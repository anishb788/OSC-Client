// Real icons (Increment 11): PNG validation (chunk walk, decompression bombs), the SD icon store, and the IconCache state machine (requests, fade-in,
// failure and retry, LRU, per-frame upload limit, dropping queued requests, refresh, offline, shutdown) against a real
// worker and a loopback server serving real PNGs from tests/fixtures/icons/. Textures are created on an SDL software
// renderer (no window). Reads tests/fixtures/ relative to the repo root, where make runs the tests.
//
// Timing rules as in worker_test.cpp: waits are conditions against a generous deadline; wall-clock bounds are only hang
// detectors. A fake clock drives the cache's `now` (fade, retry), so those never depend on real time.
#include <SDL.h>
#include <SDL_image.h>
#include <zlib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "LoopbackServer.h"
#include "core/FileUtil.h"
#include "gfx/IconCache.h"
#include "gfx/IconPng.h"
#include "gfx/IconStore.h"
#include "gfx/TextEvict.h"
#include "net/HttpPosix.h"
#include "net/Worker.h"

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char* kSlugs[] = {"WiiDonut", "WiiEAS", "wiimc", "radiow", "rumbler"};

static std::string slurp(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}
static std::string fixture(const std::string& slug) { return slurp("tests/fixtures/icons/" + slug + ".png"); }
static bool exists(const std::string& p) {
	struct stat st;
	return ::stat(p.c_str(), &st) == 0;
}
static int64_t nowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------------------------------------ PNG validation

static void testPng() {
	for (const char* s : kSlugs) {
		std::string b = fixture(s);
		CHECK(!b.empty());
		int w = 0, h = 0;
		CHECK(iconpng::checkHeader(b, w, h) == iconpng::Verdict::Ok && w == 128 && h == 48);
		iconpng::SurfacePtr surf;
		CHECK(iconpng::decode(b, surf) == iconpng::Verdict::Ok);
		CHECK(surf && surf->w == 128 && surf->h == 48 && surf->format->format == SDL_PIXELFORMAT_RGBA32);
	}
	const std::string good = fixture("wiimc");
	iconpng::SurfacePtr s;
	CHECK(iconpng::decode("", s) == iconpng::Verdict::Empty && !s);
	CHECK(iconpng::decode("<html><body>404 Not Found, and a long enough page to pass for an icon</body></html>", s) == iconpng::Verdict::NotPng);
	CHECK(iconpng::decode(good.substr(0, 4), s) == iconpng::Verdict::NotPng || iconpng::decode(good.substr(0, 4), s) == iconpng::Verdict::BadHeader);
	CHECK(iconpng::decode(good.substr(0, 20), s) == iconpng::Verdict::BadHeader);   // signature fine, header cut
	CHECK(iconpng::decode(good.substr(0, 200), s) == iconpng::Verdict::BadStructure && !s);   // header fine, data truncated
	CHECK(iconpng::decode(good.substr(0, good.size() - 30), s) == iconpng::Verdict::BadStructure);
	std::string flipped = good;
	flipped[good.size() / 2] ^= 0x55;   // a bit-flipped sector: the zlib/CRC check catches it
	CHECK(iconpng::decode(flipped, s) != iconpng::Verdict::Ok);

	// Dimensions: patched into the IHDR (the CRC is then wrong, but the limit check must refuse before any decoder runs).
	auto withDims = [&](uint32_t w, uint32_t h) {
		std::string b = good;
		for (int i = 0; i < 4; i++) {
			b[16 + i] = char(w >> (24 - 8 * i));
			b[20 + i] = char(h >> (24 - 8 * i));
		}
		return b;
	};
	CHECK(iconpng::decode(withDims(60000, 60000), s) == iconpng::Verdict::BadDimensions);   // a decompression bomb
	CHECK(iconpng::decode(withDims(0xFFFFFFFFu, 1), s) == iconpng::Verdict::BadDimensions);
	CHECK(iconpng::decode(withDims(257, 48), s) == iconpng::Verdict::BadDimensions);
	CHECK(iconpng::decode(withDims(0, 48), s) == iconpng::Verdict::BadDimensions);
	CHECK(iconpng::decode(withDims(64, 48), s) != iconpng::Verdict::Ok);   // lies about its size
	// A real, valid, but oversized image.
	{
		TempDir t;
		SDL_Surface* big = SDL_CreateRGBSurfaceWithFormat(0, 300, 10, 32, SDL_PIXELFORMAT_RGBA32);
		IMG_SavePNG(big, t.file("big.png").c_str());
		SDL_FreeSurface(big);
		std::string b = slurp(t.file("big.png"));
		CHECK(!b.empty() && iconpng::decode(b, s) == iconpng::Verdict::BadDimensions);
	}
	// A huge byte count is refused by size alone.
	std::string huge(iconpng::kMaxFileBytes + 1, 'x');
	std::memcpy(&huge[0], good.data(), 64);
	CHECK(iconpng::decode(huge, s) == iconpng::Verdict::TooBig);
	std::string atCap(iconpng::kMaxFileBytes, 'x');
	std::memcpy(&atCap[0], good.data(), 64);
	CHECK(iconpng::decode(atCap, s) != iconpng::Verdict::TooBig);
}

// ------------------------------------------------------------------------------------------------ crafted PNGs

static void be32put(std::string& s, uint32_t v) {
	for (int i = 3; i >= 0; i--) s.push_back(char(v >> (8 * i)));
}
static std::string chunk(const std::string& type, const std::string& data) {
	std::string out;
	be32put(out, uint32_t(data.size()));
	out += type;
	out += data;
	uLong c = crc32(0, reinterpret_cast<const Bytef*>(type.data()), 4);
	c = crc32(c, reinterpret_cast<const Bytef*>(data.data()), uInt(data.size()));
	be32put(out, uint32_t(c));
	return out;
}
static const std::string kSig("\x89PNG\r\n\x1a\n", 8);
static std::string ihdrData(uint32_t w, uint32_t h, int depth, int ct, bool il) {
	std::string d;
	be32put(d, w);
	be32put(d, h);
	d.push_back(char(depth));
	d.push_back(char(ct));
	d.push_back(0);
	d.push_back(0);
	d.push_back(il ? 1 : 0);
	return d;
}
static std::string assemble(const std::vector<std::string>& chunks) {
	std::string out = kSig;
	for (const auto& c : chunks) out += c;
	return out;
}
// A zlib stream of `raw`; syncRows flushes after every row of the given lengths (a streaming encoder).
static std::string deflateRaw(const std::string& raw, int level, const std::vector<size_t>* rows = nullptr) {
	z_stream zs;
	std::memset(&zs, 0, sizeof zs);
	deflateInit(&zs, level);
	std::string out;
	unsigned char buf[16384];
	auto pump = [&](int flush) {
		int rc;
		do {
			zs.next_out = buf;
			zs.avail_out = sizeof buf;
			rc = deflate(&zs, flush);
			out.append(reinterpret_cast<char*>(buf), sizeof buf - zs.avail_out);
		} while (zs.avail_out == 0 || (flush == Z_FINISH && rc != Z_STREAM_END));
	};
	size_t pos = 0;
	if (rows) {
		for (size_t n : *rows) {
			zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.data())) + pos;
			zs.avail_in = uInt(n);
			pos += n;
			pump(Z_SYNC_FLUSH);
		}
	}
	zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.data())) + pos;
	zs.avail_in = uInt(raw.size() - pos);
	pump(Z_FINISH);
	deflateEnd(&zs);
	return out;
}
// A zlib stream that inflates to `n` zero bytes, built without holding them: about 1000:1.
static std::string deflateZeros(uint64_t n) {
	z_stream zs;
	std::memset(&zs, 0, sizeof zs);
	deflateInit(&zs, 9);
	static const std::string zero(65536, '\0');
	std::string out;
	unsigned char buf[16384];
	for (uint64_t left = n;;) {
		const bool last = left <= zero.size();
		const size_t take = last ? size_t(left) : zero.size();
		zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(zero.data()));
		zs.avail_in = uInt(take);
		left -= take;
		int rc;
		do {
			zs.next_out = buf;
			zs.avail_out = sizeof buf;
			rc = deflate(&zs, last ? Z_FINISH : Z_NO_FLUSH);
			out.append(reinterpret_cast<char*>(buf), sizeof buf - zs.avail_out);
		} while (zs.avail_out == 0 || (last && rc != Z_STREAM_END));
		if (last) break;
	}
	deflateEnd(&zs);
	return out;
}

struct PngOpts {
	uint32_t w = 16, h = 16;
	int depth = 8, ct = 6;
	bool il = false;
	int level = 6;
	size_t idatParts = 1;
	bool syncRows = false;
	std::string beforeIdat, afterIdat;   // extra chunks, already built
};
static int channelsOf(int ct) { return ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : 4; }
// The inflated scanlines for the image, filter byte 0 per row, and each row's length.
static std::string rawScanlines(const PngOpts& o, std::vector<size_t>& rows) {
	std::string raw;
	const int bpp = channelsOf(o.ct) * o.depth;
	auto pass = [&](uint32_t pw, uint32_t ph) {
		if (!pw || !ph) return;
		const size_t rb = 1 + (size_t(pw) * bpp + 7) / 8;
		for (uint32_t y = 0; y < ph; y++) {
			raw.push_back(0);
			for (size_t i = 1; i < rb; i++) raw.push_back(char((raw.size() * 131 + y * 7) & 0xFF));
			rows.push_back(rb);
		}
	};
	if (!o.il) {
		pass(o.w, o.h);
	} else {
		static const unsigned xs[7] = {0, 4, 0, 2, 0, 1, 0}, ys[7] = {0, 0, 4, 0, 2, 0, 1}, dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
		for (int i = 0; i < 7; i++) pass(o.w > xs[i] ? (o.w - xs[i] + dx[i] - 1) / dx[i] : 0, o.h > ys[i] ? (o.h - ys[i] + dy[i] - 1) / dy[i] : 0);
	}
	return raw;
}
static std::string makePngWith(const PngOpts& o, const std::string& z) {
	std::vector<std::string> cs{chunk("IHDR", ihdrData(o.w, o.h, o.depth, o.ct, o.il))};
	if (o.ct == 3) {
		std::string pal;
		for (int i = 0; i < (1 << o.depth); i++) { pal.push_back(char(i * 5)); pal.push_back(char(i * 3)); pal.push_back(char(255 - i)); }
		cs.push_back(chunk("PLTE", pal));
	}
	std::string extra = o.beforeIdat;
	const size_t parts = std::max<size_t>(o.idatParts, 1);
	std::string body = kSig + cs[0] + (cs.size() > 1 ? cs[1] : "") + extra;
	const size_t each = (z.size() + parts - 1) / parts;
	for (size_t i = 0; i < z.size() || i == 0; i += std::max<size_t>(each, 1)) body += chunk("IDAT", z.substr(i, each));
	return body + o.afterIdat + chunk("IEND", "");
}
static std::string makePng(const PngOpts& o) {
	std::vector<size_t> rows;
	const std::string raw = rawScanlines(o, rows);
	return makePngWith(o, deflateRaw(raw, o.level, o.syncRows ? &rows : nullptr));
}
static double msSince(std::chrono::steady_clock::time_point t) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

static void expectDecodes(const char* what, const std::string& png, int w, int h) {
	iconpng::SurfacePtr s;
	const iconpng::Verdict v = iconpng::decode(png, s);
	if (v != iconpng::Verdict::Ok || !s || s->w != w || s->h != h || s->format->format != SDL_PIXELFORMAT_RGBA32) {
		std::printf("FAIL decode of %s: %s\n", what, iconpng::name(v));
		failures++;
	}
}

static void testCraftedPng() {
	// Everything a legitimate encoder can produce still decodes: bit depths, colour types, interlacing, split IDATs,
	// stored blocks and a sync flush after every row.
	struct Case { const char* name; PngOpts o; };
	std::vector<Case> cases;
	auto add = [&](const char* n, uint32_t w, uint32_t h, int depth, int ct, bool il, int level = 6, size_t parts = 1, bool sync = false) {
		PngOpts o;
		o.w = w; o.h = h; o.depth = depth; o.ct = ct; o.il = il; o.level = level; o.idatParts = parts; o.syncRows = sync;
		cases.push_back({n, o});
	};
	add("rgba8", 128, 48, 8, 6, false);
	add("rgb16", 40, 30, 16, 2, false);
	add("gray1", 33, 17, 1, 0, false);
	add("gray+alpha16", 20, 20, 16, 4, false);
	add("palette4", 50, 40, 4, 3, false);
	add("palette8", 31, 9, 8, 3, false);
	add("interlaced rgba8", 33, 17, 8, 6, true);
	add("interlaced palette2", 29, 13, 2, 3, true);
	add("interlaced rgb16", 7, 5, 16, 2, true);   // some of the 7 passes are empty
	add("interlaced 1x1", 1, 1, 8, 6, true);
	add("split IDAT", 64, 48, 8, 6, false, 6, 7);
	add("stored blocks", 64, 64, 8, 6, false, 0);
	add("sync flush per row", 48, 40, 8, 6, false, 0, 1, true);
	add("sync flush per row, compressed", 48, 40, 8, 6, false, 6, 1, true);
	add("sync flush per row, interlaced palette", 21, 21, 1, 3, true, 0, 1, true);
	add("sync flush per row, 1px wide", 1, 200, 8, 0, false, 0, 1, true);
	for (const Case& c : cases) {
		const std::string png = makePng(c.o);
		expectDecodes(c.name, png, int(c.o.w), int(c.o.h));
		// The size limit is never what let it through: IDAT is within the bound, and the bound is near the raw size.
		int w, h;
		CHECK(iconpng::checkHeader(png, w, h) == iconpng::Verdict::Ok);
	}
	CHECK(iconpng::rawImageBytes(16, 16, 8, 6, false) == 16 * (1 + 64));
	CHECK(iconpng::rawImageBytes(8, 8, 8, 6, true) == (1 * (1 + 4)) + (1 * (1 + 4)) + (1 * (1 + 8)) + (2 * (1 + 8)) + (2 * (1 + 16)) + (4 * (1 + 16)) + (4 * (1 + 32)));
	CHECK(iconpng::rawImageBytes(1, 1, 8, 6, true) == 1 + 4);   // only pass 1 has a pixel
	CHECK(iconpng::maxIdatBytes(16, 16, 7, 6, false) == 0);     // not a PNG format

	// APNG: acTL / fcTL / fdAT after the default image. libpng ignores the animation, so the first frame decodes.
	{
		PngOpts o;
		o.w = 32; o.h = 24;
		std::string act, fc, fd;
		be32put(act, 2); be32put(act, 0);
		be32put(fc, 0); be32put(fc, 32); be32put(fc, 24); be32put(fc, 0); be32put(fc, 0); fc += std::string("\0\x0a\0\x64\0\0", 6);
		o.beforeIdat = chunk("acTL", act) + chunk("fcTL", fc);
		be32put(fd, 1);
		std::vector<size_t> rows;
		fd += deflateRaw(rawScanlines(o, rows), 6);
		o.afterIdat = chunk("fcTL", fc) + chunk("fdAT", fd) + chunk("fdAT", fd);
		const std::string apng = makePng(o);
		expectDecodes("apng", apng, 32, 24);
		int w, h;
		CHECK(iconpng::checkHeader(apng, w, h) == iconpng::Verdict::Ok);
	}

	// ---- bomb 1: a tiny image whose IDAT inflates to 60 MB (the review's case: 16x16, 250 MB, 382 ms on x86). Refused by
	// the chunk walk alone, so no decompression happens at all.
	{
		PngOpts o;
		const std::string bomb = makePngWith(o, deflateZeros(60ull << 20));
		CHECK(bomb.size() <= iconpng::kMaxFileBytes && bomb.size() > 40000);
		int w = 0, h = 0;
		const auto t0 = std::chrono::steady_clock::now();
		CHECK(iconpng::checkHeader(bomb, w, h) == iconpng::Verdict::TooMuchData);   // the walk, with no inflate
		iconpng::SurfacePtr s;
		CHECK(iconpng::decode(bomb, s) == iconpng::Verdict::TooMuchData && !s);
		CHECK(msSince(t0) < 100);   // 60 MB of inflate would take far longer, and on a Wii seconds
	}
	// ---- bomb 2: a big image (256x256 RGBA: its legitimate IDAT bound is bigger than the whole file cap, so the length
	// check cannot help). The bounded inflate stops after raw size + one 4 KB buffer, not after 50 MB.
	{
		PngOpts o;
		o.w = 256; o.h = 256;
		const std::string bomb = makePngWith(o, deflateZeros(50ull << 20));
		CHECK(bomb.size() <= iconpng::kMaxFileBytes);
		int w = 0, h = 0;
		CHECK(iconpng::checkHeader(bomb, w, h) == iconpng::Verdict::Ok && w == 256 && h == 256);   // looks fine to the walk
		iconpng::SurfacePtr s;
		const auto t0 = std::chrono::steady_clock::now();
		CHECK(iconpng::decode(bomb, s) == iconpng::Verdict::TooMuchData && !s);
		CHECK(msSince(t0) < 100);
	}
	// A stream that is one byte too long or too short for the header, or whose adler32 is wrong.
	{
		PngOpts o;
		o.w = 64; o.h = 32;
		std::vector<size_t> rows;
		std::string raw = rawScanlines(o, rows);
		iconpng::SurfacePtr s;
		CHECK(iconpng::decode(makePngWith(o, deflateRaw(raw + "x", 6)), s) == iconpng::Verdict::TooMuchData);
		CHECK(iconpng::decode(makePngWith(o, deflateRaw(raw.substr(0, raw.size() - 1), 6)), s) == iconpng::Verdict::BadData);
		std::string z = deflateRaw(raw, 6);
		z[z.size() - 1] ^= 1;   // the adler32
		CHECK(iconpng::decode(makePngWith(o, z), s) == iconpng::Verdict::BadData);
		CHECK(iconpng::decode(makePngWith(o, z.substr(0, z.size() / 2)), s) == iconpng::Verdict::BadData);   // a stream that just ends
	}
	// ---- bomb 3: compressed ancillary chunks. The decision: such chunks are never inflated. decode() hands libpng a copy
	// with only IHDR, PLTE, tRNS, IDAT and IEND, so an iCCP / zTXt / iTXt bomb costs nothing, and the image still loads
	// (icons have no use for a colour profile). Rejecting the file instead would turn away real PNGs from editors that
	// embed a profile. The file size cap (64 KB) bounds the walk over the chunk either way.
	for (const char* type : {"iCCP", "zTXt", "iTXt"}) {
		PngOpts o;
		o.w = 128; o.h = 48;
		const std::string z = deflateZeros(60ull << 20);
		std::string data = std::string("icc") + '\0' + '\0' + z;
		if (std::string(type) == "iTXt") data = std::string("k") + '\0' + '\1' + '\0' + "en" + '\0' + "k" + '\0' + z;
		o.beforeIdat = chunk(type, data);
		const std::string png = makePng(o);
		CHECK(png.size() <= iconpng::kMaxFileBytes);
		const auto t0 = std::chrono::steady_clock::now();
		expectDecodes(type, png, 128, 48);
		CHECK(msSince(t0) < 1000);   // a hang detector: inflating 60 MB takes longer than that under a sanitizer
	}

	// ---- chunk order and shape
	const std::string ih = chunk("IHDR", ihdrData(8, 8, 8, 6, false));
	std::vector<size_t> rows;
	PngOpts o8;
	o8.w = 8; o8.h = 8;
	const std::string z = deflateRaw(rawScanlines(o8, rows), 6);
	const std::string half1 = chunk("IDAT", z.substr(0, z.size() / 2)), half2 = chunk("IDAT", z.substr(z.size() / 2));
	const std::string idat = chunk("IDAT", z), iend = chunk("IEND", ""), text = chunk("tEXt", std::string("k\0v", 3));
	auto verdictOf = [&](const std::string& png) { int w, h; return iconpng::checkHeader(png, w, h); };
	CHECK(verdictOf(assemble({ih, idat, iend})) == iconpng::Verdict::Ok);
	CHECK(verdictOf(assemble({ih, text, half1, half2, text, iend})) == iconpng::Verdict::Ok);
	CHECK(verdictOf(assemble({ih, half1, text, half2, iend})) == iconpng::Verdict::BadStructure);   // IDATs not consecutive
	CHECK(verdictOf(assemble({ih, ih, idat, iend})) == iconpng::Verdict::BadStructure);              // a second IHDR
	CHECK(verdictOf(assemble({text, ih, idat, iend})) == iconpng::Verdict::BadHeader);               // IHDR not first
	CHECK(verdictOf(assemble({ih, idat})) == iconpng::Verdict::BadStructure);                        // no IEND
	CHECK(verdictOf(assemble({ih, iend})) == iconpng::Verdict::BadStructure);                        // no image data
	CHECK(verdictOf(assemble({ih, iend, idat})) == iconpng::Verdict::BadStructure);                  // IEND before IDAT
	CHECK(verdictOf(assemble({ih, idat, chunk("PLTE", "abc"), iend})) == iconpng::Verdict::BadStructure);   // PLTE after IDAT
	CHECK(verdictOf(assemble({ih, idat, chunk("IEND", "x")})) == iconpng::Verdict::BadStructure);    // IEND with data
	// Data after IEND: a chunk, a stray byte, a whole second image.
	CHECK(verdictOf(assemble({ih, idat, iend, text})) == iconpng::Verdict::BadStructure);
	CHECK(verdictOf(assemble({ih, idat, iend}) + "x") == iconpng::Verdict::BadStructure);
	CHECK(verdictOf(assemble({ih, idat, iend}) + assemble({ih, idat, iend})) == iconpng::Verdict::BadStructure);
	// A chunk length running past the end of the file, and a cut-off CRC.
	{
		std::string bad = assemble({ih, idat, iend});
		bad[8 + 25 + 3] = char(0xFF);   // the IDAT length's low byte, now far past the end
		CHECK(verdictOf(bad) == iconpng::Verdict::BadStructure);
		const std::string ok = assemble({ih, idat, iend});
		CHECK(verdictOf(ok.substr(0, ok.size() - 2)) == iconpng::Verdict::BadStructure);
		std::string huge = assemble({ih});
		be32put(huge, 0xFFFFFFF0u);
		huge += "IDAT";
		CHECK(verdictOf(huge) == iconpng::Verdict::BadStructure);
	}
	// Bad IHDR fields: an impossible depth for the colour type, compression and filter methods, interlace method.
	CHECK(verdictOf(assemble({chunk("IHDR", ihdrData(8, 8, 16, 3, false)), idat, iend})) == iconpng::Verdict::BadHeader);
	CHECK(verdictOf(assemble({chunk("IHDR", ihdrData(8, 8, 8, 5, false)), idat, iend})) == iconpng::Verdict::BadHeader);
	{
		std::string d = ihdrData(8, 8, 8, 6, false);
		d[10] = 1;
		CHECK(verdictOf(assemble({chunk("IHDR", d), idat, iend})) == iconpng::Verdict::BadHeader);
		d = ihdrData(8, 8, 8, 6, false);
		d[12] = 2;
		CHECK(verdictOf(assemble({chunk("IHDR", d), idat, iend})) == iconpng::Verdict::BadHeader);
	}
	// Chunk count: kMaxChunks in all is fine, one more is not (small chunks, so the file stays well under the size cap).
	{
		std::vector<std::string> cs{ih, idat};
		for (size_t i = 0; i < iconpng::kMaxChunks - 3; i++) cs.push_back(text);
		cs.push_back(iend);
		const std::string at = assemble(cs);
		CHECK(at.size() < iconpng::kMaxFileBytes && verdictOf(at) == iconpng::Verdict::Ok);
		cs.insert(cs.begin() + 2, text);
		CHECK(verdictOf(assemble(cs)) == iconpng::Verdict::BadStructure);
		// Thousands of empty chunks: refused by count long before the end of the file.
		std::vector<std::string> many{ih, idat};
		for (int i = 0; i < 5000; i++) many.push_back(chunk("tEXt", ""));
		many.push_back(iend);
		const std::string tooMany = assemble(many);
		CHECK(tooMany.size() < iconpng::kMaxFileBytes && verdictOf(tooMany) == iconpng::Verdict::BadStructure);
	}
	// The bound sits exactly where documented: a file one byte over it is refused, at it passes the walk.
	{
		PngOpts o;
		o.w = 16; o.h = 16;
		const uint64_t bound = iconpng::maxIdatBytes(16, 16, 8, 6, false);
		const std::string at = makePngWith(o, std::string(size_t(bound), 'z')), over = makePngWith(o, std::string(size_t(bound) + 1, 'z'));
		CHECK(verdictOf(at) == iconpng::Verdict::Ok && verdictOf(over) == iconpng::Verdict::TooMuchData);
	}
	// And the fixtures (real icons from the server) stay inside the 64 KB cap with room to spare.
	for (const char* sl : kSlugs) CHECK(fixture(sl).size() * 4 < iconpng::kMaxFileBytes);
}

// ------------------------------------------------------------------------------------------------ the SD store

static std::string dirIn(const TempDir& t, const char* name) {
	std::string d = t.file(name);
	makeDirs(d);
	return d;
}

static void testStore() {
	TempDir t;
	const std::string cache = dirIn(t, "cache");
	const std::string a = fixture("wiimc");
	std::string out;
	CHECK(iconstore::read(cache, "wiimc", 0, 1 << 20, out) == iconstore::Read::Missing);
	CHECK(iconstore::write(cache, "wiimc", a));
	CHECK(exists(cache + "/icons/wiimc.png") && !exists(cache + "/icons/wiimc.png.tmp"));
	CHECK(iconstore::read(cache, "wiimc", a.size(), 1 << 20, out) == iconstore::Read::Ok && out == a);
	CHECK(iconstore::read(cache, "wiimc", 0, 1 << 20, out) == iconstore::Read::Ok);
	CHECK(iconstore::read(cache, "wiimc", a.size() + 1, 1 << 20, out) == iconstore::Read::SizeMismatch && out.empty());
	CHECK(iconstore::read(cache, "wiimc", 0, 100, out) == iconstore::Read::TooLarge && out.empty());
	// Never a path outside the icons directory.
	CHECK(iconstore::read(cache, "../cache", 0, 1 << 20, out) == iconstore::Read::BadSlug);
	CHECK(!iconstore::write(cache, "../evil", a) && !iconstore::write(cache, "", a) && !iconstore::write(cache, "a/b", a));
	CHECK(iconstore::pathOf(cache, "con").empty());   // a reserved device name

	// Replacing an existing file on a filesystem that cannot rename over one (libfat): the old file is parked and restored.
	FileOps libfat;
	libfat.rename = [](const char* from, const char* to) -> int {
		struct stat st;
		if (::stat(to, &st) == 0) return -1;
		return std::rename(from, to);
	};
	const std::string b = fixture("radiow");
	CHECK(iconstore::write(cache, "wiimc", b, libfat));
	CHECK(iconstore::read(cache, "wiimc", 0, 1 << 20, out) == iconstore::Read::Ok && out == b);
	CHECK(!exists(cache + "/icons/wiimc.png.bak") && !exists(cache + "/icons/wiimc.png.tmp"));
	// A failure to commit leaves the old file untouched and no tmp.
	FileOps broken;
	broken.rename = [](const char*, const char*) -> int { return -1; };
	CHECK(!iconstore::write(cache, "wiimc", a, broken));
	CHECK(iconstore::read(cache, "wiimc", 0, 1 << 20, out) == iconstore::Read::Ok && out == b);
	CHECK(!exists(cache + "/icons/wiimc.png.tmp"));

	CHECK(iconstore::remove(cache, "wiimc") && !exists(cache + "/icons/wiimc.png"));
	CHECK(iconstore::remove(cache, "wiimc"));   // already gone is fine

	// Pruning: unknown slugs, .tmp and .bak go; other files and kept slugs stay. Steps are bounded.
	for (const char* s : {"WiiDonut", "WiiEAS", "gone1", "gone2", "gone3"}) iconstore::write(cache, s, a);
	{ std::ofstream(cache + "/icons/leftover.png.tmp") << "x"; std::ofstream(cache + "/icons/old.png.bak") << "x"; std::ofstream(cache + "/icons/notes.txt") << "mine"; }
	iconstore::Pruner p(cache, {"wiidonut", "WiiEAS"});   // case-insensitive
	size_t steps = 0;
	while (p.step(2)) steps++;
	CHECK(steps >= 2);   // 5 files to delete, 2 per step
	CHECK(p.removed() == 5);
	CHECK(exists(cache + "/icons/WiiDonut.png") && exists(cache + "/icons/WiiEAS.png") && exists(cache + "/icons/notes.txt"));
	CHECK(!exists(cache + "/icons/gone1.png") && !exists(cache + "/icons/leftover.png.tmp") && !exists(cache + "/icons/old.png.bak"));
	iconstore::Pruner none(t.file("nonexistent"), {"x"});
	CHECK(!none.step(5) && none.removed() == 0);
}

// ------------------------------------------------------------------------------------------------ the cache

struct Env {
	LoopbackServer server;
	TempDir dir;
	std::string cacheDir;
	SDL_Surface* canvas = nullptr;
	SDL_Renderer* renderer = nullptr;
	net::Worker worker{[] { return makePosixTransport(); }, false};
	double now = 100;
	uint32_t gen = 1;

	Env() {
		cacheDir = dir.file("cache");
		makeDirs(cacheDir);
		canvas = SDL_CreateRGBSurfaceWithFormat(0, 64, 64, 32, SDL_PIXELFORMAT_RGBA32);
		renderer = SDL_CreateSoftwareRenderer(canvas);
		CHECK(worker.start());
		for (const char* s : kSlugs) serve(s);
	}
	~Env() {
		worker.stop();
		SDL_DestroyRenderer(renderer);
		SDL_FreeSurface(canvas);
	}
	void serve(const std::string& slug, const std::string& body = {}) {
		const std::string b = body.empty() ? fixture(slug) : body;
		server.addRawRoute("/" + slug + "/icon.png",
		                   "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " + std::to_string(b.size()) + "\r\nConnection: close\r\n\r\n" + b);
	}
	std::string url(const std::string& slug) const { return server.url("/" + slug + "/icon.png"); }
	int requestsFor(const std::string& slug) {
		int n = 0;
		for (const auto& r : server.requests())
			if (r.find("GET /" + slug + "/icon.png ") == 0) n++;
		return n;
	}
	IconCacheConfig config() const {
		IconCacheConfig c;
		c.cacheDir = cacheDir;
		c.fetchTimeoutMs = 3000;
		return c;
	}
	// One frame as App::run does it: results, beginFrame, the draws, endFrame. Returns after a short real sleep.
	void frame(IconCache& ic, const std::function<void()>& draws = nullptr) {
		worker.drainResults();
		now += 1.0 / 60;
		ic.beginFrame(now, gen);
		if (draws) draws();
		ic.endFrame();
		usleep(1500);
	}
	bool until(IconCache& ic, int timeoutMs, const std::function<void()>& draws, const std::function<bool()>& pred) {
		const int64_t end = nowMs() + timeoutMs;
		while (nowMs() < end) {
			frame(ic, draws);
			if (pred()) return true;
		}
		return pred();
	}
	// Frames until every Icon and Background job (writes included) has finished.
	void settle(IconCache& ic, const std::function<void()>& draws = nullptr) {
		until(ic, 5000, draws, [&] { return worker.pending() == 0; });
		frame(ic, draws);
	}
};

static std::optional<IconCache::View> ask(Env& e, IconCache& ic, const std::string& slug, uint64_t size = 0) {
	return ic.get(slug, e.url(slug), size, e.now);
}

static void testLoadFadeAndSdWrite() {
	Env e;
	IconCache ic(&e.worker, e.renderer, e.config());
	const std::string body = fixture("wiimc");
	std::optional<IconCache::View> v = ask(e, ic, "wiimc", body.size());
	CHECK(!v);   // a miss is a placeholder, and the request is queued
	CHECK(e.until(ic, 5000, [&] { v = ask(e, ic, "wiimc", body.size()); }, [&] { return v.has_value(); }));
	CHECK(v && v->tex && v->w == 128 && v->h == 48);
	CHECK(v && v->alpha < 0.2f);   // just shown: the fade-in starts at 0
	const double shown = e.now;
	// The fade runs from the first drawn frame, over cfg.fadeSec of the cache's own clock.
	e.now = shown + 0.10;
	v = ask(e, ic, "wiimc", body.size());
	CHECK(v && v->alpha > 0.4f && v->alpha < 0.6f);
	e.now = shown + 1.0;
	v = ask(e, ic, "wiimc", body.size());
	CHECK(v && v->alpha == 1.0f);
	e.settle(ic);
	// The SD write happened (Background job) and holds exactly the server's bytes.
	CHECK(slurp(e.cacheDir + "/icons/wiimc.png") == body);
	CHECK(!exists(e.cacheDir + "/icons/wiimc.png.tmp"));
	CHECK(e.requestsFor("wiimc") == 1);
	IconCache::Stats s = ic.stats();
	CHECK(s.fetched == 1 && s.fromSd == 0 && s.textures == 1 && s.textureBytes == 128 * 48 * 4);
}

static void testSecondRunUsesSd() {
	Env e;
	const std::string body = fixture("radiow");
	{
		IconCache ic(&e.worker, e.renderer, e.config());
		std::optional<IconCache::View> v;
		CHECK(e.until(ic, 5000, [&] { v = ask(e, ic, "radiow", body.size()); }, [&] { return v.has_value(); }));
		e.settle(ic, [&] { ask(e, ic, "radiow", body.size()); });
	}
	CHECK(e.requestsFor("radiow") == 1);
	// A new cache (a new launch) over the same directory: no request reaches the server.
	IconCache ic2(&e.worker, e.renderer, e.config());
	std::optional<IconCache::View> v;
	CHECK(e.until(ic2, 5000, [&] { v = ask(e, ic2, "radiow", body.size()); }, [&] { return v.has_value(); }));
	CHECK(e.requestsFor("radiow") == 1);
	CHECK(ic2.stats().fromSd == 1 && ic2.stats().fetched == 0);

	// A size change in the catalog: the cached file no longer counts and the server is asked again.
	IconCache ic3(&e.worker, e.renderer, e.config());
	std::optional<IconCache::View> v3;
	CHECK(e.until(ic3, 5000, [&] { v3 = ask(e, ic3, "radiow", body.size() + 7); }, [&] { return v3.has_value(); }));
	CHECK(e.requestsFor("radiow") == 2 && ic3.stats().fetched == 1);
}

static void testCorruptCacheIsReplaced() {
	Env e;
	const std::string body = fixture("rumbler");
	makeDirs(e.cacheDir + "/icons");
	// The right size but garbage, and a second one that is a truncated PNG.
	{ std::ofstream f(e.cacheDir + "/icons/rumbler.png", std::ios::binary); f << std::string(body.size(), 'Z'); }
	{ std::ofstream f(e.cacheDir + "/icons/WiiEAS.png", std::ios::binary); const std::string b = fixture("WiiEAS"); f << b.substr(0, b.size() / 2); }
	IconCache ic(&e.worker, e.renderer, e.config());
	std::optional<IconCache::View> v, v2;
	CHECK(e.until(ic, 5000, [&] { v = ask(e, ic, "rumbler", body.size()); v2 = ask(e, ic, "WiiEAS", 0); }, [&] { return v && v2; }));
	CHECK(e.requestsFor("rumbler") == 1 && e.requestsFor("WiiEAS") == 1);   // both refetched, nothing shown from the garbage
	e.settle(ic, [&] { ask(e, ic, "rumbler", body.size()); ask(e, ic, "WiiEAS", 0); });
	CHECK(slurp(e.cacheDir + "/icons/rumbler.png") == body);   // and replaced by good files
	CHECK(slurp(e.cacheDir + "/icons/WiiEAS.png") == fixture("WiiEAS"));
	// An oversized file on the SD card is deleted without being read into memory.
	{ std::ofstream f(e.cacheDir + "/icons/wiimc.png", std::ios::binary); f << std::string(iconpng::kMaxFileBytes + 10, 'x'); }
	IconCache ic2(&e.worker, e.renderer, e.config());
	std::optional<IconCache::View> v3;
	CHECK(e.until(ic2, 5000, [&] { v3 = ask(e, ic2, "wiimc", 0); }, [&] { return v3.has_value(); }));
	CHECK(e.requestsFor("wiimc") == 1);
}

static void testFailuresAndRetry() {
	Env e;
	IconCache ic(&e.worker, e.renderer, e.config());
	// 404: a permanent failure, requested once however often it is drawn.
	auto draw404 = [&] { ask(e, ic, "nosuchapp", 123); };
	CHECK(e.until(ic, 5000, draw404, [&] { return ic.stats().failed == 1; }));
	for (int i = 0; i < 60; i++) e.frame(ic, draw404);
	CHECK(e.requestsFor("nosuchapp") == 1 && ic.stats().requested == 1 && !ask(e, ic, "nosuchapp", 123));
	// Not a PNG, although the server said 200: failed for good, and nothing lands on the SD card.
	e.serve("html", "<html>Not Found, please try again later, this page is long enough</html>");
	auto drawHtml = [&] { ask(e, ic, "html", 0); };
	CHECK(e.until(ic, 5000, drawHtml, [&] { return ic.stats().failed == 2; }));
	e.settle(ic, drawHtml);
	CHECK(e.requestsFor("html") == 1 && !exists(e.cacheDir + "/icons/html.png"));
	// An oversized body is refused by the sink (no 256 KB+ buffer) and fails for good.
	e.serve("big", std::string(iconpng::kMaxFileBytes + 1000, 'x'));
	auto drawBig = [&] { ask(e, ic, "big", 0); };
	CHECK(e.until(ic, 5000, drawBig, [&] { return ic.stats().failed == 3; }));
	for (int i = 0; i < 30; i++) e.frame(ic, drawBig);
	CHECK(e.requestsFor("big") == 1);

	// A Refresh (a new generation) clears the failures: each is asked for once more.
	e.gen = 2;
	CHECK(e.until(ic, 5000, [&] { draw404(); drawHtml(); }, [&] { return e.requestsFor("nosuchapp") == 2 && e.requestsFor("html") == 2; }));

	// A transient failure (connection refused) retries after retrySec, at most maxAttempts times in all.
	IconCacheConfig cfg = e.config();
	cfg.retrySec = 30;
	IconCache rc(&e.worker, e.renderer, cfg);
	auto drawDead = [&] { rc.get("deadhost", "http://127.0.0.1:1/deadhost/icon.png", 0, e.now); };
	CHECK(e.until(rc, 5000, drawDead, [&] { return rc.stats().failed == 1; }));
	for (int i = 0; i < 30; i++) e.frame(rc, drawDead);
	CHECK(rc.stats().requested == 1);   // not before the retry time
	e.now += 31;
	CHECK(e.until(rc, 5000, drawDead, [&] { return rc.stats().failed == 2; }));
	CHECK(rc.stats().requested == 2);
	e.now += 31;
	CHECK(e.until(rc, 5000, drawDead, [&] { return rc.stats().failed == 3; }));
	e.now += 31;
	for (int i = 0; i < 30; i++) e.frame(rc, drawDead);
	CHECK(rc.stats().requested == 3);   // the third failure was the last try
}

static void testUploadLimit() {
	Env e;
	IconCacheConfig cfg = e.config();
	cfg.uploadsPerFrame = 2;
	// From the SD card: those jobs end in one step each, with no hop through the main thread in between.
	for (const char* s : kSlugs) CHECK(iconstore::write(e.cacheDir, s, fixture(s)));
	IconCache ic(&e.worker, e.renderer, cfg);
	for (const char* s : kSlugs) ask(e, ic, s, 0);
	// Let every read finish and every result arrive, with no beginFrame in between.
	const int64_t end = nowMs() + 5000;
	while (e.worker.pending() > 0 && nowMs() < end) {
		e.worker.drainResults();
		usleep(2000);
	}
	e.worker.drainResults();
	CHECK(ic.stats().uploaded == 0);
	ic.beginFrame(e.now += 0.016, e.gen);
	CHECK(ic.stats().uploaded == 2);   // never more than uploadsPerFrame textures in one frame
	ic.beginFrame(e.now += 0.016, e.gen);
	CHECK(ic.stats().uploaded == 4);
	ic.beginFrame(e.now += 0.016, e.gen);
	CHECK(ic.stats().uploaded == 5 && ic.stats().textures == 5 && ic.stats().inFlight == 0);
	for (const char* s : kSlugs) CHECK(ask(e, ic, s, 0).has_value());
}

static void loadOne(Env& e, IconCache& ic, const std::string& slug) {
	std::optional<IconCache::View> v;
	CHECK(e.until(ic, 5000, [&] { v = ask(e, ic, slug, 0); }, [&] { return v.has_value(); }));
}

static void testLru() {
	Env e;
	IconCacheConfig cfg = e.config();
	cfg.maxTextures = 3;
	{
		IconCache ic(&e.worker, e.renderer, cfg);
		for (const char* s : kSlugs) loadOne(e, ic, s);
		IconCache::Stats st = ic.stats();
		CHECK(st.textures == 3 && st.evictions == 2 && st.uploaded == 5);
		// The least recently drawn went first: the last three loaded are still there, the first two are not.
		CHECK(ask(e, ic, "rumbler", 0) && ask(e, ic, "radiow", 0) && ask(e, ic, "wiimc", 0));
		CHECK(!ask(e, ic, "WiiDonut", 0));
		e.settle(ic);
		// Drawing makes an icon recent: touch wiimc, then load one more, and wiimc survives while radiow is evicted.
		const uint64_t before = ic.stats().evictions;
		auto drawKeep = [&] { ask(e, ic, "wiimc", 0); ask(e, ic, "WiiEAS", 0); };
		CHECK(e.until(ic, 5000, drawKeep, [&] { return ic.stats().textures == 3 && ic.stats().evictions > before; }));
		CHECK(ic.stats().textures <= 3);
		// An evicted icon comes back from the SD card, not the network.
		const int reqs = e.requestsFor("WiiDonut");
		loadOne(e, ic, "WiiDonut");
		CHECK(e.requestsFor("WiiDonut") == reqs && ic.stats().fromSd >= 1);
	}
	// The byte budget bounds it too (a catalog with bigger icons): two 128x48 textures fit, a third does not.
	cfg.maxTextures = 128;
	cfg.maxTextureBytes = 2 * 128 * 48 * 4 + 100;
	IconCache bc(&e.worker, e.renderer, cfg);
	for (const char* s : kSlugs) loadOne(e, bc, s);
	CHECK(bc.stats().textures == 2 && bc.stats().textureBytes <= cfg.maxTextureBytes && bc.stats().evictions == 3);
}

static void testDropOnPageChange() {
	Env e;
	e.server.addStallRoute("/stall1/icon.png", 20000);
	IconCacheConfig cfg = e.config();
	cfg.fetchTimeoutMs = 2000;
	IconCache ic(&e.worker, e.renderer, cfg);
	// One request occupies the worker's single Icon slot (the server holds it open), so everything after it queues.
	ic.get("stall1", e.url("stall1"), 0, e.now);
	CHECK(e.until(ic, 5000, [&] { ic.get("stall1", e.url("stall1"), 0, e.now); }, [&] { return e.requestsFor("stall1") == 1; }));
	const char* page1[] = {"WiiDonut", "WiiEAS", "wiimc", "radiow"};
	auto drawPage1 = [&] {
		ic.get("stall1", e.url("stall1"), 0, e.now);
		for (const char* s : page1) ask(e, ic, s, 0);
	};
	for (int i = 0; i < 3; i++) e.frame(ic, drawPage1);
	CHECK(ic.stats().inFlight == 5 && ic.stats().dropped == 0);   // still wanted: nothing is dropped
	// The user pages on: only page 2's icon (and the busy one) are asked for from now on.
	auto drawPage2 = [&] {
		ic.get("stall1", e.url("stall1"), 0, e.now);
		ask(e, ic, "rumbler", 0);
	};
	CHECK(e.until(ic, 5000, drawPage2, [&] { return ic.stats().dropped == 4; }));
	CHECK(ic.stats().inFlight == 2);   // the busy download and rumbler remain
	for (const char* s : page1) CHECK(e.requestsFor(s) == 0);   // the dropped icons were never downloaded
	// The busy download times out (a transient failure), then rumbler gets its turn.
	std::optional<IconCache::View> v;
	CHECK(e.until(ic, 8000, [&] { ic.get("stall1", e.url("stall1"), 0, e.now); v = ask(e, ic, "rumbler", 0); }, [&] { return v.has_value(); }));
	CHECK(e.requestsFor("rumbler") == 1);
	for (const char* s : page1) CHECK(e.requestsFor(s) == 0);
	CHECK(ic.stats().stale == 4);   // each cancelled job reported back and was recognised as unwanted
	// Going back to page 1 asks again, and the icons now load.
	std::optional<IconCache::View> w;
	CHECK(e.until(ic, 5000, [&] { w = ask(e, ic, "wiimc", 0); }, [&] { return w.has_value(); }));
	CHECK(e.requestsFor("wiimc") == 1);
}

static void testRefreshKeepsTextures() {
	Env e;
	IconCache ic(&e.worker, e.renderer, e.config());
	const uint64_t size = fixture("wiimc").size();
	std::optional<IconCache::View> v;
	CHECK(e.until(ic, 5000, [&] { v = ask(e, ic, "wiimc", size); }, [&] { return v.has_value(); }));
	e.settle(ic, [&] { ask(e, ic, "wiimc", size); });
	CHECK(e.requestsFor("wiimc") == 1 && ic.stats().requested == 1);
	// A Refresh: the new catalog has the same size. The texture shows at once and nothing is downloaded.
	e.now += 1;   // the first fade-in is long over
	e.gen = 2;
	e.frame(ic);
	v = ask(e, ic, "wiimc", size);
	CHECK(v && v->alpha == 1.0f);
	for (int i = 0; i < 20; i++) e.frame(ic, [&] { ask(e, ic, "wiimc", size); });
	CHECK(e.requestsFor("wiimc") == 1 && ic.stats().requested == 1);
	// Another Refresh where the icon changed size: the old picture keeps showing while the new one is fetched.
	e.gen = 3;
	e.frame(ic);
	v = ask(e, ic, "wiimc", size + 9);
	CHECK(v.has_value() && ic.stats().requested == 2);
	CHECK(e.until(ic, 5000, [&] { ask(e, ic, "wiimc", size + 9); }, [&] { return e.requestsFor("wiimc") == 2 && ic.stats().uploaded == 2; }));
	// The replacement takes over at full alpha: the placeholder never flashes back for a size-change refetch.
	v = ask(e, ic, "wiimc", size + 9);
	CHECK(v && v->alpha == 1.0f);
	for (int i = 0; i < 20; i++) e.frame(ic, [&] { ask(e, ic, "wiimc", size + 9); });
	CHECK(ic.stats().requested == 2 && ic.stats().textures == 1);   // replaced, not duplicated, and not requested again
}

static void testOfflineAndDisabled() {
	{
		Env e;
		// Cached with another size than the catalog's: offline takes it anyway.
		makeDirs(e.cacheDir + "/icons");
		const std::string b = fixture("WiiEAS");
		CHECK(iconstore::write(e.cacheDir, "WiiEAS", b));
		IconCacheConfig cfg = e.config();
		cfg.network = false;
		IconCache ic(&e.worker, e.renderer, cfg);
		std::optional<IconCache::View> v, miss;
		CHECK(e.until(ic, 5000, [&] { v = ask(e, ic, "WiiEAS", b.size() + 1); miss = ask(e, ic, "wiimc", 99); }, [&] { return v.has_value(); }));
		CHECK(e.until(ic, 5000, [&] { v = ask(e, ic, "WiiEAS", b.size() + 1); miss = ask(e, ic, "wiimc", 99); }, [&] { return ic.stats().failed == 1; }));
		for (int i = 0; i < 30; i++) e.frame(ic, [&] { ask(e, ic, "wiimc", 99); });
		CHECK(!miss && ic.stats().requested == 2 && ic.stats().fromSd == 1);
		CHECK(e.server.requests().empty());   // no network use at all
	}
	{
		Env e;
		IconCacheConfig cfg = e.config();
		cfg.enabled = false;   // mock mode
		IconCache ic(&e.worker, e.renderer, cfg);
		for (int i = 0; i < 10; i++) e.frame(ic, [&] { CHECK(!ask(e, ic, "wiimc", 0)); });
		CHECK(ic.stats().requested == 0 && e.worker.pending() == 0 && e.server.requests().empty());
		CHECK(!exists(e.cacheDir + "/icons"));
	}
	{
		Env e;   // the dev option icons=<base>: the catalog's URL is ignored
		IconCacheConfig cfg = e.config();
		cfg.urlBase = e.server.url("");
		IconCache ic(&e.worker, e.renderer, cfg);
		std::optional<IconCache::View> v;
		CHECK(e.until(ic, 5000, [&] { v = ic.get("wiimc", "http://127.0.0.1:1/never", 0, e.now); }, [&] { return v.has_value(); }));
	}
	{
		Env e;   // a slug that is not a safe file name is never requested
		IconCache ic(&e.worker, e.renderer, e.config());
		for (int i = 0; i < 5; i++) e.frame(ic, [&] { CHECK(!ic.get("../../etc/passwd", e.url("x"), 0, e.now)); CHECK(!ic.get("", e.url("x"), 0, e.now)); });
		CHECK(ic.stats().requested == 0);
	}
}

static void testShutdownWithWork() {
	Env e;
	e.server.addStallRoute("/stall1/icon.png", 20000);
	{
		IconCacheConfig cfg = e.config();
		cfg.fetchTimeoutMs = 20000;
		IconCache ic(&e.worker, e.renderer, cfg);
		ic.get("stall1", e.url("stall1"), 0, e.now);
		CHECK(e.until(ic, 5000, [&] { ic.get("stall1", e.url("stall1"), 0, e.now); }, [&] { return e.requestsFor("stall1") == 1; }));
		for (const char* s : kSlugs) ask(e, ic, s, 0);
		const int64_t t0 = nowMs();
		e.worker.stop();
		CHECK(nowMs() - t0 < 1000);   // exit with icons downloading is quick
		// Results the stopped worker produced were never drained: they are freed with the worker (leak check under ASan).
	}
	{
		// Decoded surfaces that never reach the cache: the closures free them.
		IconCache ic(&e.worker, e.renderer, e.config());   // the worker is stopped: requests fail at once
		CHECK(!ask(e, ic, "wiimc", 0));
		CHECK(ic.stats().failed == 1);
	}
	Env e2;
	{
		IconCache ic(&e2.worker, e2.renderer, e2.config());
		for (const char* s : kSlugs) ask(e2, ic, s, 0);
		const int64_t end = nowMs() + 5000;
		while (e2.worker.pending() > 0 && nowMs() < end) usleep(2000);   // finished, results undelivered
	}
}

static void testPruneJob() {
	Env e;
	const std::string b = fixture("wiimc");
	for (const char* s : {"WiiDonut", "WiiEAS", "wiimc", "stale1", "stale2"}) CHECK(iconstore::write(e.cacheDir, s, b));
	IconCache ic(&e.worker, e.renderer, e.config());
	ic.startPrune({});   // an empty catalog must never wipe the cache
	e.settle(ic);
	CHECK(exists(e.cacheDir + "/icons/stale1.png"));
	ic.startPrune({"WiiDonut", "wiimc", "WiiEAS"});
	e.settle(ic);
	CHECK(!exists(e.cacheDir + "/icons/stale1.png") && !exists(e.cacheDir + "/icons/stale2.png"));
	CHECK(exists(e.cacheDir + "/icons/WiiDonut.png") && exists(e.cacheDir + "/icons/wiimc.png") && exists(e.cacheDir + "/icons/WiiEAS.png"));
}

// ------------------------------------------------------------------------------------------------ prune steps

static void testPruneSteps() {
	TempDir t;
	const std::string cache = dirIn(t, "cache");
	const std::string a = fixture("wiimc");
	auto stepsFor = [&](int files) {
		for (int i = 0; i < files; i++) CHECK(iconstore::write(cache, "stale" + std::to_string(i), a));
		iconstore::Pruner p(cache, {"keepme"});
		// The first step is only the directory snapshot: nothing is classified or deleted yet.
		CHECK(p.step(8) && p.removed() == 0 && p.scanned() == size_t(files));
		CHECK(exists(cache + "/icons/stale0.png"));
		size_t steps = 1;
		while (p.step(8)) {
			steps++;
			CHECK(p.removed() <= steps * 8);   // never more than perStep deletions in a step
		}
		steps++;
		CHECK(p.removed() == size_t(files));
		return steps;
	};
	const size_t s50 = stepsFor(50), s100 = stepsFor(100), s200 = stepsFor(200);
	CHECK(s50 >= 50 / 8 && s100 >= 100 / 8 && s200 >= 200 / 8);   // at least one step per 8 deletions
	CHECK(s100 > s50 && s200 > s100);                            // and the count scales with the number of files
	// An empty directory (or none): done after the snapshot.
	iconstore::Pruner empty(cache, {"x"});
	CHECK(!empty.step(8) && empty.removed() == 0);
	// Classification is bounded too: 2000 files that all stay take several steps, with nothing deleted.
	for (int i = 0; i < 2000; i++) { std::ofstream(cache + "/icons/k" + std::to_string(i) + ".txt") << "x"; }
	iconstore::Pruner many(cache, {"x"});
	size_t steps = 1;
	CHECK(many.step(8));
	while (many.step(8)) steps++;
	CHECK(steps >= 2000 / (8 * 32) && many.removed() == 0 && many.scanned() == 2000);
}

// ------------------------------------------------------------------------------------------------ text cache eviction

static void testTextEvict() {
	using textevict::Candidate;
	const size_t budget = 1000, target = 750;
	std::vector<Candidate> all;
	const uint32_t frame = 100;
	// Within budget: nothing, however old.
	all = {{1, 400}, {2, 400}};
	CHECK(textevict::choose(all, 800, budget, target, frame).empty());
	// Over budget but everything was drawn last frame (lastUsed == frame - 1): nothing may go, and no work is done.
	all = {{99, 600}, {99, 600}};
	CHECK(textevict::choose(all, 1200, budget, target, frame).empty());
	CHECK(!textevict::evictable(99, frame) && textevict::evictable(98, frame));
	// Over budget with old entries: least recently used first, only as far as the target.
	all = {{50, 200}, {99, 300}, {10, 300}, {70, 200}, {99, 200}};
	auto v = textevict::choose(all, 1200, budget, target, frame);
	CHECK(v.size() == 2 && v[0] == 2 && v[1] == 0);   // frame 10, then frame 50: 1200 - 300 - 200 = 700 <= 750
	// Still over the target after every old entry went: the entries drawn last frame stay.
	all = {{10, 100}, {99, 900}};
	v = textevict::choose(all, 1000 + 1, budget, target, frame);
	CHECK(v.size() == 1 && v[0] == 0);
	// Equal ages evict in a stable order.
	all = {{5, 300}, {5, 300}, {5, 300}, {99, 300}};
	v = textevict::choose(all, 1200, budget, target, frame);
	CHECK(v.size() == 2 && v[0] == 0 && v[1] == 1);
}

// ------------------------------------------------------------------------------------------------ cache: stalls, cancel, caps

// A fetch of the whole worker with the Icon slot held by a stalled download: what the review reproduced.
static void testSdNotBehindStall() {
	Env e;
	e.server.addStallRoute("/stall1/icon.png", 20000);
	const std::string body = fixture("wiimc");
	CHECK(iconstore::write(e.cacheDir, "wiimc", body));
	IconCacheConfig cfg = e.config();
	cfg.fetchTimeoutMs = 8000;   // the production values: the stall would hold the slot for seconds
	cfg.fetchIdleMs = 4000;
	IconCache ic(&e.worker, e.renderer, cfg);
	auto drawStall = [&] { ic.get("stall1", e.url("stall1"), 0, e.now); };
	CHECK(e.until(ic, 5000, drawStall, [&] { return e.requestsFor("stall1") == 1; }));
	// stall1's download is now the active Icon job. A cached icon is requested behind it.
	std::optional<IconCache::View> v;
	const int64_t t0 = nowMs();
	CHECK(e.until(ic, 3000, [&] { drawStall(); v = ask(e, ic, "wiimc", body.size()); }, [&] { return v.has_value(); }));
	const int64_t took = nowMs() - t0;
	std::printf("icon_test: a cached icon showed %lld ms behind a stalled download\n", (long long)took);
	CHECK(v && took < 500);
	CHECK(ic.stats().fromSd == 1 && e.requestsFor("wiimc") == 0);
	// And a cached icon that does not match the catalog's size misses on SD, then waits for the (one) download slot: it is
	// not lost, and it does not hold up the SD lane.
	CHECK(iconstore::write(e.cacheDir, "radiow", fixture("radiow")));
	std::optional<IconCache::View> w, x;
	CHECK(e.until(ic, 3000, [&] { drawStall(); w = ask(e, ic, "radiow", 1); x = ask(e, ic, "rumbler", 0); }, [&] { return ic.stats().inFlight == 3; }));
	CHECK(!w && !x);
}

static void testLeavingPageCancelsActiveDownload() {
	Env e;
	e.server.addStallRoute("/stall1/icon.png", 20000);
	IconCacheConfig cfg = e.config();
	cfg.fetchTimeoutMs = 8000;   // far longer than the test waits: only a cancel can free the slot in time
	cfg.fetchIdleMs = 4000;
	IconCache ic(&e.worker, e.renderer, cfg);
	auto drawStall = [&] { ic.get("stall1", e.url("stall1"), 0, e.now); };
	CHECK(e.until(ic, 5000, drawStall, [&] { return e.requestsFor("stall1") == 1; }));
	for (int i = 0; i < 5; i++) e.frame(ic, drawStall);
	CHECK(ic.stats().dropped == 0 && ic.stats().inFlight == 1);   // still wanted: it is left alone
	// The user leaves: page 2 asks for other icons only.
	std::optional<IconCache::View> v;
	const int64_t t0 = nowMs();
	CHECK(e.until(ic, 3000, [&] { v = ask(e, ic, "rumbler", 0); }, [&] { return v.has_value(); }));
	const int64_t took = nowMs() - t0;
	std::printf("icon_test: the next page's icon showed %lld ms after the stalled download was abandoned\n", (long long)took);
	CHECK(v && took < 2500);   // not the 4 s idle timeout, let alone the 8 s total
	CHECK(ic.stats().dropped >= 1);
	CHECK(e.requestsFor("stall1") == 1 && e.requestsFor("rumbler") == 1);
	// The cancelled job's result arrives later and is recognised as stale, not as a failure of stall1.
	e.settle(ic, [&] { ask(e, ic, "rumbler", 0); });
	CHECK(ic.stats().failed == 0 && ic.stats().stale >= 1 && ic.stats().inFlight == 0);
	// Coming back asks again (a new request, which stalls again: it is not remembered as failed).
	CHECK(e.until(ic, 5000, drawStall, [&] { return e.requestsFor("stall1") == 2; }));
}

// Ten Refreshes while the server is not answering, with the loading screen in between (no icon is asked for while it
// shows). Once the server answers, every icon on the page appears.
static void testRefreshesDuringStallConverge() {
	Env e;
	e.server.addStallRoute("/stall1/icon.png", 20000);
	IconCacheConfig cfg = e.config();
	cfg.fetchTimeoutMs = 1500;
	cfg.fetchIdleMs = 1500;
	IconCache ic(&e.worker, e.renderer, cfg);
	const char* page[] = {"stall1", "wiimc", "radiow", "WiiEAS"};
	auto drawPage = [&] { for (const char* s : page) ask(e, ic, s, 0); };
	e.server.addStallRoute("/wiimc/icon.png", 20000);   // everything stalls at first
	e.server.addStallRoute("/radiow/icon.png", 20000);
	e.server.addStallRoute("/WiiEAS/icon.png", 20000);
	for (int r = 0; r < 10; r++) {
		for (int i = 0; i < 6; i++) e.frame(ic, drawPage);
		e.gen++;   // the Refresh; the LoadingScreen shows, so nothing is asked for
		for (int i = 0; i < 20; i++) e.frame(ic);
	}
	CHECK(ic.stats().textures == 0);
	// The server answers.
	for (const char* s : page) e.serve(s, fixture(s[0] == 's' ? "wiimc" : s));
	e.gen++;
	CHECK(e.until(ic, 10000, [&] { drawPage(); e.now += 0.25; }, [&] { return ic.stats().textures == 4; }));
	for (const char* s : page) CHECK(ask(e, ic, s, 0).has_value());
}

// 30 icons of about 61 KB each are fetched faster than the Background class gets to write them: only about 1 MB of
// writes may queue, the rest are shown but skipped, and every icon still shows.
static void testWriteQueueCap() {
	Env e;
	PngOpts o;
	o.w = 128; o.h = 120; o.level = 0;
	const std::string big = makePng(o);
	CHECK(big.size() > 60000 && big.size() <= iconpng::kMaxFileBytes);
	std::vector<std::string> slugs;
	for (int i = 0; i < 30; i++) {
		slugs.push_back("big" + std::to_string(i));
		e.serve(slugs.back(), big);
	}
	IconCache ic(&e.worker, e.renderer, e.config());
	auto draw = [&] { for (const auto& s : slugs) ask(e, ic, s, 0); };
	CHECK(e.until(ic, 20000, draw, [&] { return ic.stats().textures == 30; }));
	e.settle(ic, draw);
	size_t written = 0;
	for (const auto& s : slugs) written += exists(e.cacheDir + "/icons/" + s + ".png");
	const uint64_t skipped = ic.stats().writesSkipped;
	std::printf("icon_test: %zu of 30 icons written, %llu skipped by the 1 MB queue cap\n", written, (unsigned long long)skipped);
	CHECK(skipped >= 1 && written + skipped == 30);
	CHECK(written * big.size() <= (1u << 20) + big.size() * 4);   // a few more can pass while earlier writes finish
	for (const auto& s : slugs) CHECK(!exists(e.cacheDir + "/icons/" + s + ".png.tmp"));
}

static std::atomic<int> g_renames{0};
static int failingRename(const char*, const char*) {
	g_renames++;
	return -1;
}

// A card that refuses writes: three failures in a row and the cache stops trying; icons keep showing.
static void testWriteFailuresDisableWrites() {
	Env e;
	IconCacheConfig cfg = e.config();
	cfg.fileOps.rename = &failingRename;
	g_renames = 0;
	IconCache ic(&e.worker, e.renderer, cfg);
	for (int i = 0; i < 3; i++) {
		loadOne(e, ic, kSlugs[i]);
		e.settle(ic, [&] { for (int k = 0; k <= i; k++) ask(e, ic, kSlugs[k], 0); });
	}
	const int attempts = g_renames.load();
	CHECK(attempts >= 3);   // each of the three writes got as far as the commit
	for (int i = 3; i < 5; i++) {
		loadOne(e, ic, kSlugs[i]);
		e.settle(ic, [&] { for (int k = 0; k < 5; k++) ask(e, ic, kSlugs[k], 0); });
	}
	CHECK(g_renames.load() == attempts);   // the fourth and fifth were not even attempted
	CHECK(ic.stats().textures == 5);
	for (const char* s : kSlugs) CHECK(!exists(e.cacheDir + "/icons/" + s + ".png") && !exists(e.cacheDir + "/icons/" + s + ".png.tmp"));
}

// The prune job and icon writes share the worker's Background lane; neither may hurt the other.
static void testPruneAndWritesInterleave() {
	Env e;
	const std::string b = fixture("wiimc");
	for (int i = 0; i < 80; i++) CHECK(iconstore::write(e.cacheDir, "stale" + std::to_string(i), b));
	IconCache ic(&e.worker, e.renderer, e.config());
	std::set<std::string> keep;
	for (const char* s : kSlugs) keep.insert(s);
	ic.startPrune(keep);
	auto draw = [&] { for (const char* s : kSlugs) ask(e, ic, s, 0); };
	CHECK(e.until(ic, 10000, draw, [&] { return ic.stats().textures == 5; }));
	e.settle(ic, draw);
	for (int i = 0; i < 80; i++) CHECK(!exists(e.cacheDir + "/icons/stale" + std::to_string(i) + ".png"));
	for (const char* s : kSlugs) CHECK(slurp(e.cacheDir + "/icons/" + s + ".png") == fixture(s));   // written whole, never pruned
	// (A prune step and a write step both run on the one worker thread and never overlap, so the prune's snapshot can
	// not catch a write's .tmp half done.)
}

// A server that sends a decompression bomb as an icon: refused as a failed icon, quickly, and never cached.
static void testBombFromServer() {
	Env e;
	PngOpts o;
	e.serve("bomb", makePngWith(o, deflateZeros(60ull << 20)));
	IconCache ic(&e.worker, e.renderer, e.config());
	const int64_t t0 = nowMs();
	CHECK(e.until(ic, 5000, [&] { ask(e, ic, "bomb", 0); }, [&] { return ic.stats().failed == 1; }));
	CHECK(nowMs() - t0 < 2000 && ic.stats().textures == 0);
	e.settle(ic, [&] { ask(e, ic, "bomb", 0); });
	CHECK(!exists(e.cacheDir + "/icons/bomb.png") && e.requestsFor("bomb") == 1);
}

int main() {
	alarm(120);   // a hang kills the run instead of blocking CI
	setenv("SDL_VIDEODRIVER", "dummy", 0);
	if (SDL_Init(SDL_INIT_VIDEO) != 0 || !(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) {
		std::printf("SDL/SDL_image init failed: %s\n", SDL_GetError());
		return 1;
	}
	testPng();
	testStore();
	testLoadFadeAndSdWrite();
	testSecondRunUsesSd();
	testCorruptCacheIsReplaced();
	testFailuresAndRetry();
	testUploadLimit();
	testLru();
	testDropOnPageChange();
	testRefreshKeepsTextures();
	testOfflineAndDisabled();
	testShutdownWithWork();
	testPruneJob();
	testCraftedPng();
	testPruneSteps();
	testTextEvict();
	testSdNotBehindStall();
	testLeavingPageCancelsActiveDownload();
	testRefreshesDuringStallConverge();
	testWriteQueueCap();
	testWriteFailuresDisableWrites();
	testPruneAndWritesInterleave();
	testBombFromServer();
	IMG_Quit();
	SDL_Quit();
	if (failures) {
		std::printf("icon_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("icon_test: all passed\n");
	return 0;
}

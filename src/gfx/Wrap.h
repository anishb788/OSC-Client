#pragma once
// Word wrapping with no SDL and no fonts: the caller supplies the width measure, so the host tests can use a fake
// monospace font. Text::wrap runs it to completion; DetailScreen steps it a few milliseconds per frame, so a 27 KB
// description from the live catalog never stalls a frame on the Wii.
//
// Each word is measured once and widths are summed (line + one space + word), instead of re-measuring "line word"
// for every candidate, which made long descriptions quadratic. Kerning across the joint is ignored.
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace textwrap {

using WidthFn = std::function<int(std::string_view)>;

// Byte index just past the UTF-8 character that starts at `pos` (a lone continuation byte counts as one character).
inline size_t charEnd(std::string_view s, size_t pos) {
	if (pos >= s.size()) return s.size();
	pos++;
	while (pos < s.size() && ((unsigned char)s[pos] & 0xC0) == 0x80) pos++;
	return pos;
}

class Wrapper {
public:
	// maxLines 0 = unlimited. '\r' is dropped (it has no glyph).
	Wrapper(WidthFn width, std::string_view text, int maxW, size_t maxLines = 0)
	    : width_(std::move(width)), maxW_(maxW), maxLines_(maxLines) {
		text_.reserve(text.size());
		for (char c : text) if (c != '\r') text_ += c;
		spaceW_ = width_(" ");
	}

	// Wraps until the text is finished, maxLines is reached, or `yield` (checked every 32 words) returns true.
	// Returns done().
	bool step(const std::function<bool()>& yield = nullptr) {
		unsigned words = 0;
		while (!done_) {
			if (!inPara_ && !startParagraph()) continue;
			while (i_ < pe_) {
				if (yield && (++words & 31) == 0 && yield()) return false;
				if (!word()) return true;
			}
			// End of the paragraph: its last line.
			inPara_ = false;
			if (!line_.empty() && push(std::move(line_), pos_)) return true;
			line_.clear();
			lineW_ = 0;
		}
		return true;
	}

	bool done() const { return done_; }
	// True when maxLines was reached with text left over (the caller marks the last line with an ellipsis).
	bool truncated() const { return truncated_; }
	const std::vector<std::string>& lines() const { return lines_; }

private:
	// Begins the next paragraph; returns false when it was an empty one (already pushed as a blank line) or the
	// text is over.
	bool startParagraph() {
		if (pos_ > text_.size()) { done_ = true; return false; }
		size_t nl = text_.find('\n', pos_);
		pb_ = i_ = pos_;
		pe_ = nl == std::string::npos ? text_.size() : nl;
		pos_ = nl == std::string::npos ? text_.size() + 1 : nl + 1;
		line_.clear();
		lineW_ = 0;
		if (pb_ == pe_) { push(std::string(), pos_); return false; }
		inPara_ = true;
		return true;
	}

	// One word. Returns false when the line limit ended the wrap.
	bool word() {
		const size_t start = i_;
		size_t sp = text_.find(' ', i_);
		if (sp == std::string::npos || sp > pe_) sp = pe_;
		std::string_view w = std::string_view(text_).substr(i_, sp - i_);
		i_ = sp < pe_ ? sp + 1 : pe_;

		if (w.empty()) { // a run of spaces: keep one separator, as the old code did
			if (!line_.empty()) { line_ += ' '; lineW_ += spaceW_; }
			return true;
		}
		const int ww = width_(w);
		const int need = line_.empty() ? ww : lineW_ + spaceW_ + ww;
		if (need <= maxW_) {
			if (!line_.empty()) line_ += ' ';
			line_ += w;
			lineW_ = need;
			return true;
		}
		if (!line_.empty()) {
			if (push(std::move(line_), start)) return false;
			line_.clear();
			lineW_ = 0;
		}
		if (ww <= maxW_) { line_ = std::string(w); lineW_ = ww; return true; }

		// A single word wider than the line: hard-break it, at least one whole character per line (a multibyte glyph
		// wider than maxW still goes on a line of its own instead of looping forever).
		size_t off = 0;
		while (off < w.size()) {
			size_t end = off;
			int sum = 0;
			while (end < w.size()) {
				size_t next = charEnd(w, end);
				int cw = width_(w.substr(end, next - end));
				if (end > off && sum + cw > maxW_) break;
				sum += cw;
				end = next;
			}
			if (end < w.size()) {
				if (push(std::string(w.substr(off, end - off)), start + end)) return false;
			} else {
				line_ = std::string(w.substr(off));
				lineW_ = sum;
			}
			off = end;
		}
		return true;
	}

	// Adds a line. `rest` is the byte offset where the unconsumed text starts. Returns true when that was the last
	// line allowed (then done_ is set, and truncated_ if any visible text follows).
	bool push(std::string line, size_t rest) {
		lines_.push_back(std::move(line));
		if (maxLines_ == 0 || lines_.size() < maxLines_) return false;
		done_ = true;
		for (size_t k = rest; k < text_.size(); k++)
			if (text_[k] != ' ' && text_[k] != '\n') { truncated_ = true; break; }
		return true;
	}

	WidthFn width_;
	int maxW_;
	size_t maxLines_;
	int spaceW_ = 0;
	std::string text_;
	std::vector<std::string> lines_;
	size_t pos_ = 0;      // start of the next paragraph
	size_t pb_ = 0, pe_ = 0, i_ = 0; // the current paragraph [pb_, pe_) and the next word in it
	std::string line_;
	int lineW_ = 0;
	bool inPara_ = false, done_ = false, truncated_ = false;
};

} // namespace textwrap

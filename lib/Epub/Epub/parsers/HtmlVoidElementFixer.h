#pragma once

#include <cstddef>
#include <cstdint>

// Streaming byte rewriter that lets a strict XML parser accept HTML void elements
// written without a self-closing slash: `<meta charset="utf-8">` becomes
// `<meta charset="utf-8"/>`, and end tags of void elements (`</br>`) are dropped.
// C0 control characters XML forbids (all but tab, LF, CR) become spaces.
// Comments, CDATA, processing instructions, declarations, quoted attribute values
// and <script>/<style> contents pass through untouched. Input may be split at any
// byte; state carries across feed() calls.
class HtmlVoidElementFixer {
 public:
  // Bytes held back between calls at most: an unresolved "</" plus a void-length name.
  static constexpr size_t MAX_HELD = 8;

  // Extra space feed() may need beyond its input length. Each inserted '/' needs a
  // distinct `<xx>` (>= 4 bytes) except one tag carried in from the previous chunk.
  static constexpr size_t growthFor(const size_t len) { return len / 4 + MAX_HELD + 2; }

  // Writes the rewritten bytes to `out` and returns how many were written (at most
  // len + growthFor(len)). `out` may alias the input when in >= out + growthFor(len):
  // output never overtakes unread input.
  size_t feed(const char* in, size_t len, char* out);

  // Releases bytes held back at end of input (at most MAX_HELD).
  size_t finish(char* out);

  void reset() { *this = HtmlVoidElementFixer(); }

 private:
  enum class State : uint8_t {
    Text,
    Lt,
    StartTagName,
    StartTagAttrs,
    EndTagName,
    EndTagRest,
    VoidEndTag,
    Bang,
    BangDash,
    Comment,
    CdataOpen,
    Cdata,
    Declaration,
    Pi,
    RawText,
    RawEndCheck,
  };
  // Longest tag name that matters (void elements, script, style).
  static constexpr uint8_t MAX_NAME_LEN = 6;

  bool nameIsVoid() const;
  uint8_t nameRawKind() const;
  // Returns false when `c` must be reprocessed in the new state.
  bool step(char c, char* out, size_t& o);

  State state = State::Text;
  char name[MAX_NAME_LEN] = {};
  uint8_t nameLen = 0;  // MAX_NAME_LEN + 1 means "longer than any tracked name"
  char quote = 0;
  char lastSignificant = 0;
  uint8_t counter = 0;  // dashes, brackets, CDATA-prefix progress or raw end-tag progress
  uint8_t rawKind = 0;  // 1 = script, 2 = style
};

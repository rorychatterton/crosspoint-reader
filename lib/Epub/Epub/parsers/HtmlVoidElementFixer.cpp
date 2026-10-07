#include "HtmlVoidElementFixer.h"

#include <cstring>

namespace {
constexpr const char* VOID_ELEMENTS[] = {"area",  "base", "br",   "col",   "embed",  "hr",    "img",
                                         "input", "link", "meta", "param", "source", "track", "wbr"};
constexpr const char* RAW_ELEMENTS[] = {"script", "style"};
constexpr const char CDATA_OPEN[] = "[CDATA[";

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool isNameStart(const char c) {
  const auto u = static_cast<unsigned char>(c);
  return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u == ':' || u >= 0x80;
}

char toLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool nameEquals(const char* name, const uint8_t len, const char* target) {
  if (std::strlen(target) != len) return false;
  for (uint8_t i = 0; i < len; i++) {
    if (toLower(name[i]) != target[i]) return false;
  }
  return true;
}
}  // namespace

bool HtmlVoidElementFixer::nameIsVoid() const {
  if (nameLen > MAX_NAME_LEN) return false;
  for (const char* v : VOID_ELEMENTS) {
    if (nameEquals(name, nameLen, v)) return true;
  }
  return false;
}

uint8_t HtmlVoidElementFixer::nameRawKind() const {
  if (nameLen > MAX_NAME_LEN) return 0;
  for (uint8_t i = 0; i < 2; i++) {
    if (nameEquals(name, nameLen, RAW_ELEMENTS[i])) return i + 1;
  }
  return 0;
}

bool HtmlVoidElementFixer::step(const char c, char* out, size_t& o) {
  switch (state) {
    case State::Text:
      if (c == '<') {
        state = State::Lt;  // held until we know it is not a void end tag
      } else {
        out[o++] = c;
      }
      return true;

    case State::Lt:
      if (c == '/') {
        state = State::EndTagName;
        nameLen = 0;
        return true;
      }
      out[o++] = '<';
      if (c == '!') {
        out[o++] = c;
        state = State::Bang;
      } else if (c == '?') {
        out[o++] = c;
        state = State::Pi;
        counter = 0;
      } else if (isNameStart(c)) {
        out[o++] = c;
        name[0] = c;
        nameLen = 1;
        state = State::StartTagName;
      } else {
        state = State::Text;
        return false;
      }
      return true;

    case State::StartTagName:
      if (isSpace(c) || c == '/' || c == '>') {
        state = State::StartTagAttrs;
        quote = 0;
        lastSignificant = 0;
        return false;
      }
      if (nameLen < MAX_NAME_LEN) {
        name[nameLen] = c;
      }
      if (nameLen <= MAX_NAME_LEN) nameLen++;
      out[o++] = c;
      return true;

    case State::StartTagAttrs:
      if (quote != 0) {
        if (c == quote) quote = 0;
        out[o++] = c;
        return true;
      }
      if (c == '>') {
        const bool selfClosed = lastSignificant == '/';
        if (!selfClosed && nameIsVoid()) {
          out[o++] = '/';
        }
        out[o++] = c;
        rawKind = selfClosed ? 0 : nameRawKind();
        state = rawKind != 0 ? State::RawText : State::Text;
        counter = 0;
        return true;
      }
      if (c == '"' || c == '\'') quote = c;
      if (!isSpace(c)) lastSignificant = c;
      out[o++] = c;
      return true;

    case State::EndTagName: {
      const bool nameEnded = isSpace(c) || c == '>' || c == '/';
      if (!nameEnded && nameLen < MAX_NAME_LEN) {
        name[nameLen++] = c;
        return true;
      }
      if (nameEnded && nameIsVoid()) {
        state = c == '>' ? State::Text : State::VoidEndTag;
        return true;
      }
      // Not a void end tag (or longer than any void name): release the held bytes.
      out[o++] = '<';
      out[o++] = '/';
      for (uint8_t i = 0; i < nameLen; i++) out[o++] = name[i];
      out[o++] = c;
      state = c == '>' ? State::Text : State::EndTagRest;
      return true;
    }

    case State::EndTagRest:
      out[o++] = c;
      if (c == '>') state = State::Text;
      return true;

    case State::VoidEndTag:
      if (c == '>') state = State::Text;
      return true;

    case State::Bang:
      if (c == '-') {
        out[o++] = c;
        state = State::BangDash;
        return true;
      }
      if (c == '[') {
        out[o++] = c;
        state = State::CdataOpen;
        counter = 1;
        return true;
      }
      state = State::Declaration;
      quote = 0;
      counter = 0;
      return false;

    case State::BangDash:
      if (c == '-') {
        out[o++] = c;
        state = State::Comment;
        counter = 0;
        return true;
      }
      state = State::Declaration;
      quote = 0;
      counter = 0;
      return false;

    case State::Comment:
      out[o++] = c;
      if (c == '-') {
        if (counter < 2) counter++;
      } else if (c == '>' && counter >= 2) {
        state = State::Text;
      } else {
        counter = 0;
      }
      return true;

    case State::CdataOpen:
      if (c == CDATA_OPEN[counter]) {
        out[o++] = c;
        if (++counter == sizeof(CDATA_OPEN) - 1) {
          state = State::Cdata;
          counter = 0;
        }
        return true;
      }
      state = State::Declaration;
      quote = 0;
      counter = 0;
      return false;

    case State::Cdata:
      out[o++] = c;
      if (c == ']') {
        if (counter < 2) counter++;
      } else if (c == '>' && counter >= 2) {
        state = State::Text;
      } else {
        counter = 0;
      }
      return true;

    case State::Declaration:
      // counter tracks internal-subset bracket depth.
      out[o++] = c;
      if (quote != 0) {
        if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '[') {
        if (counter < 255) counter++;
      } else if (c == ']') {
        if (counter > 0) counter--;
      } else if (c == '>' && counter == 0) {
        state = State::Text;
      }
      return true;

    case State::Pi:
      out[o++] = c;
      if (c == '>' && counter == 1) {
        state = State::Text;
      }
      counter = c == '?' ? 1 : 0;
      return true;

    case State::RawText: {
      // counter = progress through "</" + raw element name.
      out[o++] = c;
      const char* raw = RAW_ELEMENTS[rawKind - 1];
      if (counter == 0) {
        counter = c == '<' ? 1 : 0;
      } else if (counter == 1) {
        counter = c == '/' ? 2 : (c == '<' ? 1 : 0);
      } else if (toLower(c) == raw[counter - 2]) {
        counter++;
        if (raw[counter - 2] == '\0') {
          state = State::RawEndCheck;
        }
      } else {
        counter = c == '<' ? 1 : 0;
      }
      return true;
    }

    case State::RawEndCheck:
      if (isSpace(c) || c == '>') {
        state = State::EndTagRest;
        rawKind = 0;
        return false;
      }
      state = State::RawText;
      counter = 0;
      return false;
  }
  return true;
}

size_t HtmlVoidElementFixer::feed(const char* in, const size_t len, char* out) {
  size_t o = 0;
  size_t i = 0;
  while (i < len) {
    if (step(in[i], out, o)) i++;
  }
  return o;
}

size_t HtmlVoidElementFixer::finish(char* out) {
  size_t o = 0;
  if (state == State::Lt) {
    out[o++] = '<';
  } else if (state == State::EndTagName) {
    out[o++] = '<';
    out[o++] = '/';
    for (uint8_t i = 0; i < nameLen; i++) out[o++] = name[i];
  }
  state = State::Text;
  return o;
}

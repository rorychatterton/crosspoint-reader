#include <GfxRenderer.h>
#include <expat.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "Epub/parsers/HtmlVoidElementFixer.h"

namespace {

// The spine item that failed to index on device: XHTML with an HTML5-style <meta>.
constexpr const char* UNCLOSED_META_CHAPTER = R"(<?xml version='1.0' encoding='utf-8'?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head>
    <title>The Five Greatest Warriors</title>
    <meta charset="utf-8">
    <link href="stylesheet.css" type="text/css" rel="stylesheet"/>
    <style type="text/css">
		@page { margin-bottom: 5.000000pt; margin-top: 5.000000pt; }</style>
  </head>
  <body class="calibre"><div class="calibre1">
<p class="calibre2"><span face="Calibri" class="calibre3"><span><img alt="cover_fivegreatestwarriors.jpg" src="images/00001.jpg" class="calibre4"/></span></span></p><div class="calibre1"></div>
<span></span>
</div> </body>
</html>
)";

// Rewrites `input` fed in chunks of `chunk` bytes, through a separate output buffer.
std::string fixChunked(const std::string& input, const size_t chunk) {
  HtmlVoidElementFixer fixer;
  std::string result;
  std::vector<char> out;
  for (size_t pos = 0; pos < input.size(); pos += chunk) {
    const size_t len = std::min(chunk, input.size() - pos);
    out.assign(len + HtmlVoidElementFixer::growthFor(len), '\0');
    const size_t n = fixer.feed(input.data() + pos, len, out.data());
    EXPECT_LE(n, out.size());
    result.append(out.data(), n);
  }
  char tail[HtmlVoidElementFixer::MAX_HELD];
  result.append(tail, fixer.finish(tail));
  return result;
}

// Same, but in place: input sits growthFor(len) bytes past the output start.
std::string fixInPlace(const std::string& input, const size_t chunk) {
  HtmlVoidElementFixer fixer;
  std::string result;
  std::vector<char> buf;
  for (size_t pos = 0; pos < input.size(); pos += chunk) {
    const size_t len = std::min(chunk, input.size() - pos);
    const size_t headroom = HtmlVoidElementFixer::growthFor(len);
    buf.assign(headroom + len, '\0');
    std::copy(input.begin() + pos, input.begin() + pos + len, buf.begin() + headroom);
    const size_t n = fixer.feed(buf.data() + headroom, len, buf.data());
    result.append(buf.data(), n);
  }
  char tail[HtmlVoidElementFixer::MAX_HELD];
  result.append(tail, fixer.finish(tail));
  return result;
}

// Every chunking (1 byte up to whole input), separate and in-place buffers, must agree.
void expectFixed(const std::string& input, const std::string& expected) {
  for (size_t chunk = 1; chunk <= input.size(); chunk++) {
    EXPECT_EQ(fixChunked(input, chunk), expected) << "chunk=" << chunk;
    EXPECT_EQ(fixInPlace(input, chunk), expected) << "in-place chunk=" << chunk;
  }
}

bool isWellFormed(const std::string& xml) {
  XML_Parser parser = XML_ParserCreate(nullptr);
  const bool ok = XML_Parse(parser, xml.data(), static_cast<int>(xml.size()), XML_TRUE) == XML_STATUS_OK;
  XML_ParserFree(parser);
  return ok;
}

// Runs the real chapter parser over `html` from a file; returns false on parse failure.
bool parseChapter(const std::string& html, int* pages = nullptr) {
  const std::string path = (std::filesystem::temp_directory_path() / "void_fixer_chapter.xhtml").string();
  {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(html.data(), 1, html.size(), f);
    std::fclose(f);
  }
  GfxRenderer renderer;
  int pageCount = 0;
  ChapterHtmlSlimParser parser(
      nullptr, path, renderer, 0, 1.0f, false, 0, static_cast<uint16_t>(renderer.getScreenWidth()),
      static_cast<uint16_t>(renderer.getScreenHeight()), false, false,
      [&pageCount](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) { pageCount++; }, false, "", "", 1);
  const bool ok = parser.parseAndBuildPages();
  std::filesystem::remove(path);
  if (pages) *pages = pageCount;
  return ok;
}

}  // namespace

TEST(HtmlVoidElementFixer, ClosesUnclosedVoidElements) {
  expectFixed(R"(<meta charset="utf-8"><br><hr class="x"><img src="a.jpg" >)",
              R"(<meta charset="utf-8"/><br/><hr class="x"/><img src="a.jpg" />)");
}

TEST(HtmlVoidElementFixer, CoversEveryVoidElementCaseInsensitively) {
  expectFixed("<area><base><BR><Col><embed><HR><img><input><link><META><param><source><track><wbr>",
              "<area/><base/><BR/><Col/><embed/><HR/><img/><input/><link/><META/><param/><source/><track/><wbr/>");
}

TEST(HtmlVoidElementFixer, LeavesSelfClosedAndNonVoidTagsAlone) {
  const std::string input =
      R"(<p><br/><br /><img src="a" /><brx><b>bold</b><metadata>m</metadata><colgroup></colgroup><sourcex></sourcex></brx></p>)";
  expectFixed(input, input);
}

TEST(HtmlVoidElementFixer, IgnoresGreaterThanInsideQuotedAttributes) {
  expectFixed(R"(<img alt="a > b" title='<br>'><meta content="x/">)",
              R"(<img alt="a > b" title='<br>'/><meta content="x/"/>)");
}

TEST(HtmlVoidElementFixer, DropsVoidEndTags) {
  expectFixed("<p>a<br></br>b<img src=\"x\"></IMG >c</p></brx>", "<p>a<br/>b<img src=\"x\"/>c</p></brx>");
}

TEST(HtmlVoidElementFixer, LeavesCommentsCdataScriptAndStyleUntouched) {
  const std::string input =
      "<?xml version='1.0'?><!DOCTYPE html [<!ENTITY x \"<br>\">]><html><head>"
      "<!-- <br> <meta charset=\"utf-8\"> -- > --><style type=\"text/css\">p::after { content: \"<br>\" }</style>"
      "<script>var s = '<br>';</SCRIPT ></head><body><![CDATA[ <br> ]] > ]]><p>x</p></body></html>";
  expectFixed(input, input);
}

TEST(HtmlVoidElementFixer, ReplacesForbiddenControlCharacters) {
  expectFixed(std::string("<p>Wieckowska\x0b\x0bVIDEO\x01</p>\t\r\n"), "<p>Wieckowska  VIDEO </p>\t\r\n");
}

TEST(HtmlVoidElementFixer, ReleasesHeldBytesAtEndOfInput) {
  expectFixed("a<", "a<");
  expectFixed("a</b", "a</b");
  expectFixed("a</br", "a</br");
}

TEST(HtmlVoidElementFixer, UnclosedMetaChapterIsWellFormedAfterFixing) {
  ASSERT_FALSE(isWellFormed(UNCLOSED_META_CHAPTER));
  for (const size_t chunk : {1u, 7u, 64u, 800u}) {
    EXPECT_TRUE(isWellFormed(fixChunked(UNCLOSED_META_CHAPTER, chunk))) << "chunk=" << chunk;
  }
}

TEST(HtmlVoidElementFixer, ChapterParserAcceptsUnclosedMeta) {
  int pages = 0;
  EXPECT_TRUE(parseChapter(UNCLOSED_META_CHAPTER, &pages));
  EXPECT_GE(pages, 0);
}

TEST(HtmlVoidElementFixer, ChapterParserHandlesVoidTagsAcrossReadBoundaries) {
  // Slide unclosed void tags across the parser's file-read boundary at every offset.
  for (size_t pad = 0; pad < 40; pad++) {
    std::string html = "<?xml version='1.0' encoding='utf-8'?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>";
    html += "<!--" + std::string(700 + pad, 'x') + "-->";
    for (int i = 0; i < 40; i++) html += "<meta name=\"m\" content=\"a > b\"><link rel=\"x\" href=\"y\">";
    html += "</head><body><p>One<br>two<BR>three</p>";
    for (int i = 0; i < 200; i++) html += "<p>Para<br>text <img alt=\"i\" src=\"x.jpg\"></p><hr>";
    html += "</body></html>";
    int pages = 0;
    EXPECT_TRUE(parseChapter(html, &pages)) << "pad=" << pad;
    EXPECT_GT(pages, 0) << "pad=" << pad;
  }
}

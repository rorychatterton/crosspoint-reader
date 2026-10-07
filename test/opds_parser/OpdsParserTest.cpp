#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "OpdsParser.h"

namespace {

std::vector<OpdsEntry> parseFeed(const std::string& entriesXml) {
  const std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<feed xmlns=\"http://www.w3.org/2005/Atom\">\n" +
      entriesXml + "</feed>\n";
  OpdsParser parser;
  parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
  parser.flush();
  EXPECT_FALSE(parser.error());
  return std::move(parser).getEntries();
}

std::string bookEntry(const std::string& links) {
  return "<entry><title>Book</title><id>urn:book</id>" + links + "</entry>\n";
}

std::string epubLink(const std::string& href, const std::string& extraAttrs) {
  return "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip\" href=\"" + href + "\" " +
         extraAttrs + "/>";
}

}  // namespace

TEST(OpdsParser, AcquisitionLengthBecomesEntrySize) {
  const auto entries = parseFeed(
      bookEntry(epubLink("/opds/download/6/epub/", "length=\"8993040\" mtime=\"2024-01-01T00:00:00+00:00\"")));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].type, OpdsEntryType::BOOK);
  EXPECT_EQ(entries[0].href, "/opds/download/6/epub/");
  EXPECT_EQ(entries[0].size, 8993040u);
}

TEST(OpdsParser, SizeComesFromTheSelectedLink) {
  // A derived EPUB link first, then the plain one: the plain link wins and
  // brings its own length.
  auto entries = parseFeed(
      bookEntry(epubLink("/get/kepub/6", "length=\"111\"") + epubLink("/opds/download/6/epub/", "length=\"222\"")));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].href, "/opds/download/6/epub/");
  EXPECT_EQ(entries[0].size, 222u);

  // Plain link first: a later derived link does not replace it or its size.
  entries =
      parseFeed(bookEntry(epubLink("/books/6.epub", "length=\"333\"") + epubLink("/get/kepub/6", "length=\"444\"")));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].href, "/books/6.epub");
  EXPECT_EQ(entries[0].size, 333u);

  // A selected link without a length does not inherit the replaced link's.
  entries = parseFeed(bookEntry(epubLink("/get/kepub/6", "length=\"555\"") + epubLink("/books/6.epub", "")));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].href, "/books/6.epub");
  EXPECT_EQ(entries[0].size, 0u);
}

TEST(OpdsParser, MissingOrInvalidLengthIsZero) {
  for (const char* attrs : {"", "length=\"\"", "length=\"abc\"", "length=\"-1\"", "length=\"12x\"", "length=\" 12\"",
                            "length=\"4294967296\"", "length=\"99999999999999999999999\""}) {
    const auto entries = parseFeed(bookEntry(epubLink("/books/6.epub", attrs)));
    ASSERT_EQ(entries.size(), 1u) << attrs;
    EXPECT_EQ(entries[0].size, 0u) << attrs;
  }
  const auto entries = parseFeed(bookEntry(epubLink("/books/6.epub", "length=\"4294967295\"")));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].size, 4294967295u);
}

TEST(OpdsParser, NavigationEntriesHaveNoSize) {
  const auto entries = parseFeed(
      "<entry><title>By Author</title><id>urn:nav</id>"
      "<link rel=\"subsection\" type=\"application/atom+xml;profile=opds-catalog\" href=\"/opds/authors\" "
      "length=\"123\"/></entry>\n");
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].type, OpdsEntryType::NAVIGATION);
  EXPECT_EQ(entries[0].size, 0u);
}

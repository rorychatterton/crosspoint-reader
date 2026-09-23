// Response-body completeness as reported by freeink::SecureHttpClient over a
// scripted transport. The interesting outcome is responseComplete(): the
// downloader relies on it to reject truncated firmware/EPUB bodies.
#include <gtest/gtest.h>

#include <string>

#include "SecureHttpClient.h"

namespace {

class HttpFraming : public ::testing::Test {
 protected:
  void SetUp() override { fakeSocket() = FakeSocketScript{}; }

  // Loads the canned response, issues one GET over plain http, and returns
  // the status code.
  int get(const std::string& response, bool closeAfter = true) {
    fakeSocket().response = response;
    fakeSocket().closeAfter = closeAfter;
    EXPECT_TRUE(http.begin("http://example.test/file.bin"));
    http.setTimeout(50);  // virtual ms; keeps stalled-socket loops short
    return http.GET();
  }

  freeink::SecureHttpClient http;
};

}  // namespace

// --- Content-Length --------------------------------------------------------

TEST_F(HttpFraming, ContentLengthExactIsComplete) {
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"), 200);
  EXPECT_TRUE(http.responseComplete());
  EXPECT_EQ(http.getString(), "hello");
  EXPECT_TRUE(http.hasContentLength());
  EXPECT_EQ(http.getContentLength(), 5u);
}

TEST_F(HttpFraming, ContentLengthShortIsIncomplete) {
  // Peer closes after 5 of 10 promised bytes.
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello"), 200);
  EXPECT_FALSE(http.responseComplete());
  EXPECT_EQ(http.getString(), "hello");  // partial body still delivered
  EXPECT_FALSE(http.aborted());
  EXPECT_FALSE(http.callbackAborted());
}

TEST_F(HttpFraming, ContentLengthShortOnStalledSocketTimesOut) {
  // Peer stops sending but keeps the socket open: the read deadline fires.
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello", /*closeAfter=*/false), 200);
  EXPECT_FALSE(http.responseComplete());
  EXPECT_EQ(http.getString(), "hello");
}

TEST_F(HttpFraming, ContentLengthZeroIsComplete) {
  EXPECT_EQ(get("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n"), 204);
  EXPECT_TRUE(http.responseComplete());
  EXPECT_EQ(http.getString(), "");
}

// --- Chunked ---------------------------------------------------------------

TEST_F(HttpFraming, ChunkedCompleteIsComplete) {
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"),
            200);
  EXPECT_TRUE(http.responseComplete());
  EXPECT_EQ(http.getString(), "hello world");
}

TEST_F(HttpFraming, ChunkedTruncatedBeforeTerminatorIsIncomplete) {
  // Connection drops after a full chunk but before the 0-size terminator.
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n"), 200);
  EXPECT_FALSE(http.responseComplete());
  EXPECT_EQ(http.getString(), "hello");
}

TEST_F(HttpFraming, ChunkedTruncatedMidChunkIsIncomplete) {
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nA\r\nhello"), 200);
  EXPECT_FALSE(http.responseComplete());
  EXPECT_EQ(http.getString(), "hello");
}

TEST_F(HttpFraming, ChunkedBadSizeLineIsIncomplete) {
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n"), 200);
  EXPECT_FALSE(http.responseComplete());
}

// --- Close-delimited -------------------------------------------------------

TEST_F(HttpFraming, CloseDelimitedBodyReportsCompleteEvenWhenTruncated) {
  // No Content-Length and no chunking: the body ends when the peer closes,
  // so the client has no way to tell a full body from a cut-off one and
  // reports it complete. Documents the gap; callers needing integrity must
  // verify the payload themselves (size/hash) or require framed responses.
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello"), 200);
  EXPECT_TRUE(http.responseComplete());
  EXPECT_FALSE(http.hasContentLength());
  EXPECT_EQ(http.getString(), "hello");
}

TEST_F(HttpFraming, CloseDelimitedStalledSocketIsIncomplete) {
  // Peer neither closes nor sends: the deadline fires and the body is
  // reported incomplete (the one truncation the close-delimited path sees).
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello", /*closeAfter=*/false), 200);
  EXPECT_FALSE(http.responseComplete());
}

// --- Framing edge cases ----------------------------------------------------

TEST_F(HttpFraming, UnknownTransferEncodingIsIncomplete) {
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\nContent-Length: 5\r\n\r\nhello"), 200);
  EXPECT_FALSE(http.responseComplete());
}

TEST_F(HttpFraming, HeadersCutOffIsReportedAsCompleteEmptyBody) {
  // Peer closes mid-header, before the blank line. The client treats the end
  // of input as the end of the header block, finds no framing headers, and
  // reads the (empty) body close-delimited: status 200, complete, no body.
  // Documents the gap; a caller cannot distinguish this from a genuine empty
  // close-delimited response.
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nContent-Len"), 200);
  EXPECT_TRUE(http.responseComplete());
  EXPECT_FALSE(http.hasContentLength());
  EXPECT_EQ(http.getString(), "");
}

TEST_F(HttpFraming, StatusLineCutOffReturnsTransportError) {
  EXPECT_EQ(get("HTTP/1.1 200"), -1);
  EXPECT_FALSE(http.responseComplete());
}

TEST_F(HttpFraming, IncompleteBodyClosesConnectionInsteadOfReusing) {
  // A truncated body must not leave a kept-alive socket around; the next
  // request has to reconnect rather than parse leftovers as a status line.
  EXPECT_EQ(get("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello"), 200);
  EXPECT_FALSE(http.responseComplete());
  const int before = fakeSocket().connectCalls;
  fakeSocket().response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
  EXPECT_TRUE(http.begin("http://example.test/next"));
  EXPECT_EQ(http.GET(), 200);
  EXPECT_TRUE(http.responseComplete());
  EXPECT_EQ(fakeSocket().connectCalls, before + 1);
}

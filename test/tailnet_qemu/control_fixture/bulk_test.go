package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/md5"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"strconv"
	"testing"
	"time"

	"tailscale.com/types/key"
)

// The stream must not depend on how it is chunked: the guest verifies in
// recv-sized pieces against a server that writes 4 KB at a time.
func TestBulkStreamIsChunkIndependent(t *testing.T) {
	const total = 10000
	whole := make([]byte, total)
	newBulkStream().fill(whole)

	pieced := make([]byte, 0, total)
	stream := newBulkStream()
	for _, size := range []int{1, 3, 4, 7, 1024, 1, 2, 4095} {
		if len(pieced)+size > total {
			size = total - len(pieced)
		}
		chunk := make([]byte, size)
		stream.fill(chunk)
		pieced = append(pieced, chunk...)
	}
	rest := make([]byte, total-len(pieced))
	stream.fill(rest)
	pieced = append(pieced, rest...)
	if !bytes.Equal(whole, pieced) {
		t.Fatal("chunked generation differs from one-shot generation")
	}
	// First word of xorshift32 from bulkSeed, little-endian, pins the
	// sequence the guest regenerates.
	x := bulkSeed
	x ^= x << 13
	x ^= x >> 17
	x ^= x << 5
	want := []byte{byte(x), byte(x >> 8), byte(x >> 16), byte(x >> 24)}
	if !bytes.Equal(whole[:4], want) {
		t.Fatalf("first word = %x, want %x", whole[:4], want)
	}
	if bytes.Count(whole, []byte{0}) > total/64 {
		t.Fatalf("stream looks degenerate: %d zero bytes of %d", bytes.Count(whole, []byte{0}), total)
	}
}

// /bulk must send exactly N bytes of the stream with a matching
// Content-Length, log the digest, and reject bad sizes.
func TestServeBulkLengthAndDigest(t *testing.T) {
	var logged []string
	peer := &wgPeer{name: "api-gateway", bind: &derpBind{}, logf: func(format string, args ...any) {
		logged = append(logged, fmt.Sprintf(format, args...))
	}}
	const n = 3*bulkChunk + 17
	response := httptest.NewRecorder()
	peer.serveBulk(response, httptest.NewRequest(http.MethodGet, "/bulk?bytes="+strconv.Itoa(n), nil))
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d", response.Code)
	}
	if got := response.Header().Get("Content-Length"); got != strconv.Itoa(n) {
		t.Fatalf("Content-Length = %q, want %d", got, n)
	}
	body := response.Body.Bytes()
	if len(body) != n {
		t.Fatalf("body length = %d, want %d", len(body), n)
	}
	want := make([]byte, n)
	newBulkStream().fill(want)
	if !bytes.Equal(body, want) {
		t.Fatal("body differs from the deterministic stream")
	}
	digest := fmt.Sprintf("md5=%x", md5.Sum(body))
	found := false
	for _, line := range logged {
		if bytes.Contains([]byte(line), []byte(digest)) && bytes.Contains([]byte(line), []byte("WG_FIXTURE bulk peer=api-gateway")) {
			found = true
		}
	}
	if !found {
		t.Fatalf("no bulk log line with %s in %q", digest, logged)
	}

	for _, bad := range []string{"", "0", "-1", "abc", strconv.Itoa(bulkMaxBytes + 1)} {
		response := httptest.NewRecorder()
		peer.serveBulk(response, httptest.NewRequest(http.MethodGet, "/bulk?bytes="+bad, nil))
		if response.Code != http.StatusBadRequest {
			t.Fatalf("bytes=%q status = %d, want 400", bad, response.Code)
		}
	}
}

// The impairment must drop, duplicate and reorder at roughly the configured
// rates, deliver every non-dropped frame, and release a held frame after
// its successor (or on the hold timer).
func TestImpairmentSendCounts(t *testing.T) {
	im := newImpairment(10, 10, 10, 7)
	im.holdFor = time.Hour
	dst := key.NewNode().Public()
	var sent [][]byte
	send := func(_ key.NodePublic, data []byte) error {
		sent = append(sent, append([]byte(nil), data...))
		return nil
	}
	const frames = 2000
	for i := 0; i < frames; i++ {
		if err := im.send(dst, []byte{byte(i), byte(i >> 8)}, send); err != nil {
			t.Fatal(err)
		}
	}
	im.flush()
	dropped := int(im.dropped.Load())
	duplicated := int(im.duplicated.Load())
	reordered := int(im.reordered.Load())
	for name, count := range map[string]int{"dropped": dropped, "duplicated": duplicated, "reordered": reordered} {
		if count < frames/20 || count > frames/5 {
			t.Fatalf("%s = %d of %d frames, want about 10%%", name, count, frames)
		}
	}
	if len(sent) != frames-dropped+duplicated {
		t.Fatalf("delivered %d frames, want %d-%d+%d", len(sent), frames, dropped, duplicated)
	}
	swaps := 0
	for i := 1; i < len(sent); i++ {
		if bytes.Compare(sent[i], sent[i-1]) < 0 && !bytes.Equal(sent[i], sent[i-1]) {
			swaps++
		}
	}
	if swaps == 0 {
		t.Fatal("no frame was delivered after its successor")
	}

	// A lone held frame must leave on the timer.
	timed := newImpairment(0, 0, 100, 1)
	timed.holdFor = 10 * time.Millisecond
	released := make(chan []byte, 1)
	err := timed.send(dst, []byte("lone"), func(_ key.NodePublic, data []byte) error {
		released <- data
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	select {
	case data := <-released:
		if string(data) != "lone" {
			t.Fatalf("released %q", data)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("held frame was never flushed")
	}
	if timed.reordered.Load() != 0 {
		t.Fatalf("a timer flush counted as a reorder")
	}

	if im, err := impairmentFromEnv(); err != nil || im != nil {
		t.Fatalf("clean environment gave impairment %v, err %v", im, err)
	}
	t.Setenv(envDropPct, "2")
	t.Setenv(envDupPct, "1")
	t.Setenv(envReorderPct, "1")
	t.Setenv(envImpairSeed, "42")
	fromEnv, err := impairmentFromEnv()
	if err != nil {
		t.Fatal(err)
	}
	if want := "drop_pct=2 dup_pct=1 reorder_pct=1 seed=42"; fromEnv.String() != want {
		t.Fatalf("impairment = %q, want %q", fromEnv, want)
	}
	t.Setenv(envDropPct, "101")
	if _, err := impairmentFromEnv(); err == nil {
		t.Fatal("percentage over 100 was accepted")
	}
}

// Two wireguard-go peers on a lossy relay must still deliver /bulk byte for
// byte: the in-process version of the qemu_v6_bulk_lossy scenario, with the
// impairment on the server's bind in both directions.
func TestWGPeersBulkOverLossyDerp(t *testing.T) {
	region := startTestDerp(t)
	client, err := startWGPeer("client", []netip.Addr{netip.MustParseAddr("100.64.0.1")}, region, t.Logf)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(client.close)
	impair := newImpairment(2, 1, 1, 3)
	server, err := startImpairedWGPeer("server", []netip.Addr{apiGatewayAddr}, region, impair, t.Logf)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(server.close)
	if err := client.addPeer(server); err != nil {
		t.Fatal(err)
	}
	if err := server.setReader(client.public(), readerPrefix); err != nil {
		t.Fatal(err)
	}

	const n = 256 << 10
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	tcp, err := client.tnet.DialContextTCP(ctx, netip.AddrPortFrom(apiGatewayAddr, 80))
	if err != nil {
		t.Fatal(err)
	}
	defer tcp.Close()
	_ = tcp.SetDeadline(time.Now().Add(60 * time.Second))
	if _, err := io.WriteString(tcp, "GET /bulk?bytes="+strconv.Itoa(n)+" HTTP/1.0\r\nHost: calibre.lab.qemu.test\r\n\r\n"); err != nil {
		t.Fatal(err)
	}
	response, err := http.ReadResponse(bufio.NewReader(tcp), nil)
	if err != nil {
		t.Fatal(err)
	}
	if response.ContentLength != n {
		t.Fatalf("Content-Length = %d, want %d", response.ContentLength, n)
	}
	body, err := io.ReadAll(response.Body)
	if err != nil {
		t.Fatal(err)
	}
	want := make([]byte, n)
	newBulkStream().fill(want)
	if !bytes.Equal(body, want) {
		t.Fatalf("received %d bytes that differ from the stream", len(body))
	}
	if impair.dropped.Load() == 0 || impair.duplicated.Load() == 0 || impair.reordered.Load() == 0 {
		t.Fatalf("impairment did not engage: %s", impair.stats())
	}
	t.Logf("lossy bulk ok: %s", impair.stats())
}

package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"net/url"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/tailscale/wireguard-go/conn"
	"golang.org/x/net/dns/dnsmessage"
	"tailscale.com/derp/derpserver"
	"tailscale.com/tailcfg"
	"tailscale.com/types/key"
)

// startTestDerp serves a DERP relay behind a self-signed TLS listener on a
// random port, returning the region getter derphttp needs to reach it.
func startTestDerp(t *testing.T) func() *tailcfg.DERPRegion {
	t.Helper()
	server := derpserver.New(key.NewNode(), t.Logf)
	mux := http.NewServeMux()
	mux.Handle("/derp", derpserver.Handler(server))
	ts := httptest.NewTLSServer(mux)
	t.Cleanup(func() {
		ts.Close()
		server.Close()
	})
	u, err := url.Parse(ts.URL)
	if err != nil {
		t.Fatal(err)
	}
	port, err := strconv.Atoi(u.Port())
	if err != nil {
		t.Fatal(err)
	}
	return fixtureDERPRegion(u.Hostname(), port)
}

type openBind struct {
	bind    *derpBind
	receive conn.ReceiveFunc
	public  key.NodePublic
}

func openTestBind(t *testing.T, name string, region func() *tailcfg.DERPRegion) *openBind {
	t.Helper()
	private := key.NewNode()
	bind := newDerpBind(name, private, region, t.Logf)
	fns, _, err := bind.Open(0)
	if err != nil {
		t.Fatal(err)
	}
	if len(fns) != 1 {
		t.Fatalf("Open returned %d receive funcs, want 1", len(fns))
	}
	t.Cleanup(func() { bind.Close() })
	return &openBind{bind: bind, receive: fns[0], public: private.Public()}
}

type received struct {
	data []byte
	from key.NodePublic
	err  error
}

// startReceive runs one ReceiveFunc call in the background.
func startReceive(b *openBind) <-chan received {
	results := make(chan received, 1)
	go func() {
		packets := [][]byte{make([]byte, 2048)}
		sizes := make([]int, 1)
		eps := make([]conn.Endpoint, 1)
		n, err := b.receive(packets, sizes, eps)
		if err != nil {
			results <- received{err: err}
			return
		}
		if n != 1 {
			results <- received{err: fmt.Errorf("receive returned %d packets", n)}
			return
		}
		results <- received{data: packets[0][:sizes[0]], from: eps[0].(*derpEndpoint).key}
	}()
	return results
}

// sendUntilReceived resends through the relay until the receiver reports a
// packet. A DERP client only becomes addressable once the server has
// registered it, slightly after Connect returns, and the relay drops frames
// for unknown keys, like WireGuard retrying a lost handshake.
func sendUntilReceived(t *testing.T, from *openBind, to *openBind, send func() error) ([]byte, key.NodePublic) {
	t.Helper()
	results := startReceive(to)
	deadline := time.After(10 * time.Second)
	for {
		if err := send(); err != nil {
			t.Fatal(err)
		}
		select {
		case r := <-results:
			if r.err != nil {
				t.Fatal(r.err)
			}
			return r.data, r.from
		case <-deadline:
			t.Fatalf("no packet from %s reached %s through the relay within 10s", from.bind.name, to.bind.name)
		case <-time.After(100 * time.Millisecond):
		}
	}
}

// Two binds on one relay must exchange a packet with the sender's key as the
// endpoint, skipping disco packets and honouring the send offset.
func TestDerpBindRoundTrip(t *testing.T) {
	region := startTestDerp(t)
	a := openTestBind(t, "a", region)
	b := openTestBind(t, "b", region)

	// A disco packet precedes every WireGuard frame; the receiver must only
	// ever surface the WireGuard frame, taken from the send offset.
	disco := append(append([]byte{}, discoMagic...), bytes.Repeat([]byte{0xaa}, 60)...)
	const offset = 8
	payload := []byte("wireguard-frame-for-b")
	framed := append(make([]byte, offset), payload...)
	got, from := sendUntilReceived(t, a, b, func() error {
		if err := a.bind.Send([][]byte{disco}, &derpEndpoint{key: b.public}, 0); err != nil {
			return err
		}
		return a.bind.Send([][]byte{framed}, &derpEndpoint{key: b.public}, offset)
	})
	if !bytes.Equal(got, payload) {
		t.Fatalf("b received %q, want %q", got, payload)
	}
	if from != a.public {
		t.Fatalf("b saw sender %s, want %s", from, a.public)
	}

	reply := []byte("reply-for-a")
	got, from = sendUntilReceived(t, b, a, func() error {
		return b.bind.Send([][]byte{reply}, &derpEndpoint{key: a.public}, 0)
	})
	if !bytes.Equal(got, reply) || from != b.public {
		t.Fatalf("a received %q from %s, want %q from %s", got, from, reply, b.public)
	}

	ep, err := a.bind.ParseEndpoint(b.public.UntypedHexString())
	if err != nil {
		t.Fatal(err)
	}
	if ep.(*derpEndpoint).key != b.public {
		t.Fatalf("ParseEndpoint returned %s, want %s", ep.DstToString(), b.public.UntypedHexString())
	}
	if _, _, err := a.bind.Open(0); err != conn.ErrBindAlreadyOpen {
		t.Fatalf("second Open error = %v, want ErrBindAlreadyOpen", err)
	}
	if err := a.bind.Close(); err != nil {
		t.Fatal(err)
	}
	if err := a.bind.Send([][]byte{reply}, &derpEndpoint{key: b.public}, 0); err != net.ErrClosed {
		t.Fatalf("Send after Close error = %v, want net.ErrClosed", err)
	}
}

// Two wireguard-go devices whose only transport is the relay must complete
// a handshake and carry HTTP: the in-process version of the QEMU tunnel.
func TestWGPeersExchangeHTTPOverDerp(t *testing.T) {
	region := startTestDerp(t)
	client, err := startWGPeer("client", []netip.Addr{netip.MustParseAddr("100.64.0.1")}, region, t.Logf)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(client.close)
	server, err := startWGPeer("server", []netip.Addr{labGWAddr, labRoutedAddr}, region, t.Logf)
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

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	tcp, err := client.tnet.DialContextTCP(ctx, netip.AddrPortFrom(labRoutedAddr, 80))
	if err != nil {
		t.Fatal(err)
	}
	defer tcp.Close()
	_ = tcp.SetDeadline(time.Now().Add(10 * time.Second))
	if _, err := io.WriteString(tcp, "GET / HTTP/1.0\r\nHost: calibre.lab.qemu.test\r\n\r\n"); err != nil {
		t.Fatal(err)
	}
	response, err := http.ReadResponse(bufio.NewReader(tcp), nil)
	if err != nil {
		t.Fatal(err)
	}
	body, err := io.ReadAll(response.Body)
	if err != nil {
		t.Fatal(err)
	}
	if want := "tailnet-ok server calibre.lab.qemu.test\n"; string(body) != want {
		t.Fatalf("body = %q, want %q", body, want)
	}
}

func buildQuery(t *testing.T, name string, qtype dnsmessage.Type) []byte {
	t.Helper()
	builder := dnsmessage.NewBuilder(nil, dnsmessage.Header{ID: 0x1234, RecursionDesired: true})
	if err := builder.StartQuestions(); err != nil {
		t.Fatal(err)
	}
	err := builder.Question(dnsmessage.Question{
		Name:  dnsmessage.MustNewName(name),
		Type:  qtype,
		Class: dnsmessage.ClassINET,
	})
	if err != nil {
		t.Fatal(err)
	}
	query, err := builder.Finish()
	if err != nil {
		t.Fatal(err)
	}
	return query
}

func TestAnswerDNSResolvesRoutedName(t *testing.T) {
	response, name, rcode, err := answerDNS(buildQuery(t, "Calibre.Lab.QEMU.test.", dnsmessage.TypeA))
	if err != nil {
		t.Fatal(err)
	}
	if name != dnsAnswerName || rcode != dnsmessage.RCodeSuccess {
		t.Fatalf("name=%q rcode=%v", name, rcode)
	}
	var parser dnsmessage.Parser
	header, err := parser.Start(response)
	if err != nil {
		t.Fatal(err)
	}
	if header.ID != 0x1234 || !header.Response || header.RCode != dnsmessage.RCodeSuccess {
		t.Fatalf("header = %+v", header)
	}
	if err := parser.SkipAllQuestions(); err != nil {
		t.Fatal(err)
	}
	answers, err := parser.AllAnswers()
	if err != nil {
		t.Fatal(err)
	}
	if len(answers) != 1 {
		t.Fatalf("answers = %d, want 1", len(answers))
	}
	a, ok := answers[0].Body.(*dnsmessage.AResource)
	if !ok || a.A != labRoutedAddr.As4() {
		t.Fatalf("answer = %+v, want A %s", answers[0], labRoutedAddr)
	}
}

func TestAnswerDNSReturnsNXDomainForOtherNames(t *testing.T) {
	response, name, rcode, err := answerDNS(buildQuery(t, "other.lab.qemu.test.", dnsmessage.TypeA))
	if err != nil {
		t.Fatal(err)
	}
	if name != "other.lab.qemu.test" || rcode != dnsmessage.RCodeNameError {
		t.Fatalf("name=%q rcode=%v", name, rcode)
	}
	var parser dnsmessage.Parser
	header, err := parser.Start(response)
	if err != nil {
		t.Fatal(err)
	}
	if header.RCode != dnsmessage.RCodeNameError {
		t.Fatalf("rcode = %v, want NXDOMAIN", header.RCode)
	}
	if err := parser.SkipAllQuestions(); err != nil {
		t.Fatal(err)
	}
	answers, err := parser.AllAnswers()
	if err != nil {
		t.Fatal(err)
	}
	if len(answers) != 0 {
		t.Fatalf("answers = %d, want 0", len(answers))
	}
}

func TestAnswerDNSRejectsGarbage(t *testing.T) {
	if _, _, _, err := answerDNS([]byte{1, 2, 3}); err == nil {
		t.Fatal("expected an error for a truncated query")
	}
}

// The map must carry the running peers' keys, and fetching it must hand the
// reader's key to the peers.
func TestMapAdvertisesPeerKeysAndConfiguresReader(t *testing.T) {
	region := startTestDerp(t)
	peers, err := startFixturePeers(region, nil, log.Printf)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(peers.close)
	f := newFixture()
	f.usePeers(peers)

	reader := key.NewNode().Public()
	body := []byte(`{"Version":131,"NodeKey":"` + reader.String() + `","Stream":false,"Hostinfo":{"Hostname":"crosspoint-qemu-resolver"}}`)
	request := httptest.NewRequest(http.MethodPost, "/machine/map", bytes.NewReader(body))
	response := httptest.NewRecorder()
	f.peerMap(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d", response.Code)
	}
	var peerMap struct {
		Peers []struct {
			Name string `json:"Name"`
			Key  string `json:"Key"`
		} `json:"Peers"`
	}
	if err := json.Unmarshal(response.Body.Bytes()[4:], &peerMap); err != nil {
		t.Fatal(err)
	}
	keys := map[string]string{}
	for _, peer := range peerMap.Peers {
		keys[peer.Name] = peer.Key
	}
	if keys[labGWName] != peers.labGW.nodeKey() || keys[labDNSName] != peers.labDNS.nodeKey() {
		t.Fatalf("map keys lab-gw=%s lab-dns=%s, want %s and %s", keys[labGWName], keys[labDNSName],
			peers.labGW.nodeKey(), peers.labDNS.nodeKey())
	}
	if strings.Contains(response.Body.String(), "nodekey:3333") || strings.Contains(response.Body.String(), "nodekey:5555") {
		t.Fatal("placeholder keys leaked into the map")
	}
	for _, peer := range peers.all() {
		config, err := peer.dev.IpcGet()
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(config, "public_key="+reader.UntypedHexString()) ||
			!strings.Contains(config, "allowed_ip="+readerPrefix.String()) {
			t.Fatalf("%s is not configured with the reader:\n%s", peer.name, config)
		}
	}
}

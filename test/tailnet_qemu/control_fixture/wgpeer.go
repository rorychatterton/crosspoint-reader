package main

import (
	"bytes"
	"context"
	"crypto/md5"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/netip"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/tailscale/wireguard-go/conn"
	"github.com/tailscale/wireguard-go/device"
	"github.com/tailscale/wireguard-go/tun"
	"go4.org/mem"
	"golang.org/x/net/dns/dnsmessage"
	"tailscale.com/derp"
	"tailscale.com/derp/derphttp"
	"tailscale.com/net/netmon"
	"tailscale.com/tailcfg"
	"tailscale.com/types/key"
	"tailscale.com/types/logger"
)

// Fixture peers are real userspace WireGuard endpoints (wireguard-go over
// a gVisor stack, netstack.go) whose only transport is the fixture's DERP
// relay, the same path the firmware uses. Each peer holds a node key; the
// map handler hands the reader's node key to every peer so its handshake
// initiation, relayed by DERP, completes and traffic flows through the
// tunnel.

// discoMagic prefixes Tailscale disco packets, which share the DERP channel
// with WireGuard traffic and must not reach wireguard-go.
var discoMagic = []byte{'T', 'S', 0xf0, 0x9f, 0x92, 0xac}

// Addresses the map advertises for the fixture peers.
var (
	apiGatewayAddr = netip.MustParseAddr("100.64.0.42")
	labGWAddr      = netip.MustParseAddr("100.64.0.77")
	labRoutedAddr  = netip.MustParseAddr("100.70.0.42")
	labDNSAddr     = netip.MustParseAddr("100.64.0.53")
	readerPrefix   = netip.MustParsePrefix("100.64.0.1/32")
)

const (
	// dnsAnswerName is the only name lab-dns resolves; it maps to the
	// address routed through lab-gw so name resolution and the routed data
	// path are exercised together.
	dnsAnswerName = "calibre.lab.qemu.test"
	peerMTU       = 1280
)

// fixtureDERPRegion describes the fixture's relay as a one-node region so
// derphttp dials it with InsecureForTests (the listener's cert is
// self-signed) instead of a base tls.Config, which tlsdial rejects.
func fixtureDERPRegion(host string, port int) func() *tailcfg.DERPRegion {
	region := &tailcfg.DERPRegion{
		RegionID:   9,
		RegionCode: "qemu",
		RegionName: "qemu fixture",
		Nodes: []*tailcfg.DERPNode{{
			Name:             "9a",
			RegionID:         9,
			HostName:         host,
			IPv4:             host,
			DERPPort:         port,
			InsecureForTests: true,
		}},
	}
	return func() *tailcfg.DERPRegion { return region }
}

// derpEndpoint is a conn.Endpoint that names a peer by node key; DERP has no
// addresses, only keys.
type derpEndpoint struct {
	key key.NodePublic
}

func (e *derpEndpoint) ClearSrc()           {}
func (e *derpEndpoint) SrcToString() string { return "" }
func (e *derpEndpoint) DstToString() string { return e.key.UntypedHexString() }
func (e *derpEndpoint) DstIP() netip.Addr   { return netip.Addr{} }
func (e *derpEndpoint) SrcIP() netip.Addr   { return netip.Addr{} }
func (e *derpEndpoint) DstToBytes() []byte {
	raw := e.key.Raw32()
	return raw[:]
}

// derpBind is a conn.Bind whose datagrams are DERP frames: Send relays to
// the endpoint's node key, receive yields frames addressed to this bind's
// key. wireguard-go closes and reopens the bind on Up, so the derphttp
// client is created per Open.
type derpBind struct {
	name    string
	private key.NodePrivate
	region  func() *tailcfg.DERPRegion
	logf    logger.Logf
	// impair, when set, drops, duplicates and reorders frames in both
	// directions (impair.go). nil is the clean path.
	impair *impairment

	mu     sync.Mutex
	client *derphttp.Client
	// pending is a duplicated inbound frame the next receive call delivers.
	pending *heldFrame
}

func newDerpBind(name string, private key.NodePrivate, region func() *tailcfg.DERPRegion, logf logger.Logf) *derpBind {
	return &derpBind{name: name, private: private, region: region, logf: logf}
}

func (b *derpBind) Open(port uint16) ([]conn.ReceiveFunc, uint16, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.client != nil {
		return nil, 0, conn.ErrBindAlreadyOpen
	}
	client := derphttp.NewRegionClient(b.private, b.logf, netmon.NewStatic(), b.region)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := client.Connect(ctx); err != nil {
		client.Close()
		return nil, 0, fmt.Errorf("%s: derp connect: %w", b.name, err)
	}
	b.client = client
	receive := func(packets [][]byte, sizes []int, eps []conn.Endpoint) (int, error) {
		return b.receive(client, packets, sizes, eps)
	}
	return []conn.ReceiveFunc{receive}, port, nil
}

func (b *derpBind) receive(client *derphttp.Client, packets [][]byte, sizes []int, eps []conn.Endpoint) (int, error) {
	b.mu.Lock()
	pending := b.pending
	b.pending = nil
	b.mu.Unlock()
	if pending != nil {
		n := copy(packets[0], pending.data)
		sizes[0] = n
		eps[0] = &derpEndpoint{key: pending.dst}
		return 1, nil
	}
	for {
		message, err := client.Recv()
		if err != nil {
			if errors.Is(err, derphttp.ErrClientClosed) {
				return 0, net.ErrClosed
			}
			// derphttp reconnects on the next Recv; keep wireguard-go's
			// receive routine alive instead of letting it give up.
			b.logf("WG_FIXTURE derp_recv_error peer=%s err=%v", b.name, err)
			time.Sleep(200 * time.Millisecond)
			continue
		}
		packet, ok := message.(derp.ReceivedPacket)
		if !ok {
			continue
		}
		if len(packet.Data) < 4 || bytes.HasPrefix(packet.Data, discoMagic) {
			continue
		}
		if b.impair != nil {
			if b.impair.dropIncoming() {
				continue
			}
			if b.impair.duplicateIncoming() {
				b.mu.Lock()
				b.pending = &heldFrame{dst: packet.Source, data: append([]byte(nil), packet.Data...)}
				b.mu.Unlock()
			}
		}
		n := copy(packets[0], packet.Data)
		sizes[0] = n
		eps[0] = &derpEndpoint{key: packet.Source}
		return 1, nil
	}
}

func (b *derpBind) Send(bufs [][]byte, ep conn.Endpoint, offset int) error {
	endpoint, ok := ep.(*derpEndpoint)
	if !ok {
		return conn.ErrWrongEndpointType
	}
	b.mu.Lock()
	client := b.client
	b.mu.Unlock()
	if client == nil {
		return net.ErrClosed
	}
	for _, buf := range bufs {
		if len(buf) <= offset {
			continue
		}
		if b.impair != nil {
			if err := b.impair.send(endpoint.key, buf[offset:], client.Send); err != nil {
				return err
			}
			continue
		}
		if err := client.Send(endpoint.key, buf[offset:]); err != nil {
			return err
		}
	}
	return nil
}

func (b *derpBind) Close() error {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.client == nil {
		return nil
	}
	err := b.client.Close()
	b.client = nil
	return err
}

func (b *derpBind) SetMark(uint32) error { return nil }
func (b *derpBind) BatchSize() int       { return 1 }

// ParseEndpoint accepts a node key as untyped hex or "nodekey:hex".
func (b *derpBind) ParseEndpoint(s string) (conn.Endpoint, error) {
	var public key.NodePublic
	if strings.HasPrefix(s, "nodekey:") {
		if err := public.UnmarshalText([]byte(s)); err != nil {
			return nil, err
		}
		return &derpEndpoint{key: public}, nil
	}
	public, err := key.ParseNodePublicUntyped(mem.S(s))
	if err != nil {
		return nil, err
	}
	return &derpEndpoint{key: public}, nil
}

// wgPeer is one fixture tailnet node: a wireguard-go device over a derpBind
// with a netstack that owns the peer's tailnet addresses.
type wgPeer struct {
	name    string
	private key.NodePrivate
	addrs   []netip.Addr
	dev     *device.Device
	tun     tun.Device
	tnet    *stackNet
	bind    *derpBind
	logf    logger.Logf
}

func startWGPeer(name string, addrs []netip.Addr, region func() *tailcfg.DERPRegion, logf logger.Logf) (*wgPeer, error) {
	return startImpairedWGPeer(name, addrs, region, nil, logf)
}

// startImpairedWGPeer is startWGPeer with a relay impairment (nil for the
// clean path) on the peer's DERP bind.
func startImpairedWGPeer(name string, addrs []netip.Addr, region func() *tailcfg.DERPRegion, impair *impairment,
	logf logger.Logf) (*wgPeer, error) {
	tunDevice, tnet, err := createNetTUN(addrs, peerMTU)
	if err != nil {
		return nil, fmt.Errorf("%s: netstack: %w", name, err)
	}
	private := key.NewNode()
	bind := newDerpBind(name, private, region, logf)
	bind.impair = impair
	dev := device.NewDevice(tunDevice, bind, device.NewLogger(device.LogLevelError, "WG_FIXTURE "+name+": "))
	if err := dev.IpcSet("private_key=" + private.UntypedHexString() + "\n"); err != nil {
		dev.Close()
		return nil, fmt.Errorf("%s: private key: %w", name, err)
	}
	if err := dev.Up(); err != nil {
		dev.Close()
		return nil, fmt.Errorf("%s: up: %w", name, err)
	}
	peer := &wgPeer{name: name, private: private, addrs: addrs, dev: dev, tun: tunDevice, tnet: tnet, bind: bind,
		logf: logf}
	listener, err := tnet.ListenTCP(netip.AddrPortFrom(netip.Addr{}, 80))
	if err != nil {
		dev.Close()
		return nil, fmt.Errorf("%s: listen tcp 80: %w", name, err)
	}
	go func() {
		_ = http.Serve(listener, http.HandlerFunc(peer.serveHTTP))
	}()
	logf("WG_FIXTURE peer_started name=%s node=%s addrs=%v", name, private.Public().String(), addrs)
	return peer, nil
}

func (p *wgPeer) public() key.NodePublic { return p.private.Public() }

// nodeKey is the map's "Key" value for this peer.
func (p *wgPeer) nodeKey() string { return p.public().String() }

// setReader makes the reader the device's only WireGuard peer. The reader's
// node key changes between firmware boots, so peers are replaced rather
// than accumulated. The endpoint is the reader's own key: this wireguard-go
// fork does not learn endpoints from received packets (Tailscale's
// magicsock owns them), so the reply path must be configured up front.
func (p *wgPeer) setReader(reader key.NodePublic, allowed netip.Prefix) error {
	return p.dev.IpcSet(fmt.Sprintf("replace_peers=true\npublic_key=%s\nendpoint=%s\nallowed_ip=%s\n",
		reader.UntypedHexString(), reader.UntypedHexString(), allowed))
}

// addPeer configures another fixture peer with a DERP endpoint, for tests
// that run the tunnel entirely in-process.
func (p *wgPeer) addPeer(other *wgPeer) error {
	var config strings.Builder
	fmt.Fprintf(&config, "public_key=%s\nendpoint=%s\n", other.public().UntypedHexString(),
		other.public().UntypedHexString())
	for _, addr := range other.addrs {
		fmt.Fprintf(&config, "allowed_ip=%s/32\n", addr)
	}
	return p.dev.IpcSet(config.String())
}

func (p *wgPeer) serveHTTP(w http.ResponseWriter, r *http.Request) {
	p.logf("WG_FIXTURE http peer=%s host=%s remote=%s method=%s path=%s", p.name, r.Host, r.RemoteAddr,
		r.Method, r.URL.Path)
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	if r.URL.Path == "/bulk" {
		p.serveBulk(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprintf(w, "tailnet-ok %s %s\n", p.name, r.Host)
}

const (
	// bulkSeed starts the xorshift32 stream /bulk serves. The guest holds the
	// same constant (test/tailnet_qemu/src/main.cpp) and regenerates the body
	// to compare byte for byte, so nothing has to buffer the transfer.
	bulkSeed     uint32 = 0x2545f491
	bulkMaxBytes        = 16 << 20
	bulkChunk           = 4096
)

// bulkStream emits the xorshift32 sequence from bulkSeed as little-endian
// words, byte by byte across chunk boundaries.
type bulkStream struct {
	state uint32
	word  uint32
	have  int
}

func newBulkStream() *bulkStream { return &bulkStream{state: bulkSeed} }

func (s *bulkStream) fill(dst []byte) {
	for i := range dst {
		if s.have == 0 {
			x := s.state
			x ^= x << 13
			x ^= x >> 17
			x ^= x << 5
			s.state = x
			s.word = x
			s.have = 4
		}
		dst[i] = byte(s.word)
		s.word >>= 8
		s.have--
	}
}

// serveBulk answers GET /bulk?bytes=N with N bytes of the deterministic
// stream and an exact Content-Length, written in bulkChunk pieces. The log
// line carries the MD5 the guest must match.
func (p *wgPeer) serveBulk(w http.ResponseWriter, r *http.Request) {
	n, err := strconv.Atoi(r.URL.Query().Get("bytes"))
	if err != nil || n < 1 || n > bulkMaxBytes {
		http.Error(w, fmt.Sprintf("bytes must be 1..%d", bulkMaxBytes), http.StatusBadRequest)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.Itoa(n))
	w.WriteHeader(http.StatusOK)
	started := time.Now()
	stream := newBulkStream()
	sum := md5.New()
	buf := make([]byte, bulkChunk)
	sent := 0
	for sent < n {
		chunk := min(len(buf), n-sent)
		stream.fill(buf[:chunk])
		sum.Write(buf[:chunk])
		if _, err := w.Write(buf[:chunk]); err != nil {
			p.logf("WG_FIXTURE bulk_error peer=%s sent=%d of=%d err=%v", p.name, sent, n, err)
			return
		}
		sent += chunk
	}
	if flusher, ok := w.(http.Flusher); ok {
		flusher.Flush()
	}
	p.logf("WG_FIXTURE bulk peer=%s bytes=%d md5=%x ms=%d", p.name, n, sum.Sum(nil),
		time.Since(started).Milliseconds())
	if p.bind.impair != nil {
		p.logf("WG_FIXTURE impairment_stats peer=%s %s", p.name, p.bind.impair.stats())
	}
}

// serveDNS answers UDP DNS on addr:53 inside the peer's netstack.
func (p *wgPeer) serveDNS(addr netip.Addr) error {
	pc, err := p.tnet.ListenUDP(netip.AddrPortFrom(addr, 53))
	if err != nil {
		return fmt.Errorf("%s: listen udp 53: %w", p.name, err)
	}
	go func() {
		buf := make([]byte, 512)
		for {
			n, from, err := pc.ReadFrom(buf)
			if err != nil {
				return
			}
			response, name, rcode, err := answerDNS(buf[:n])
			if err != nil {
				p.logf("WG_FIXTURE dns peer=%s from=%s error=%v", p.name, from, err)
				continue
			}
			p.logf("WG_FIXTURE dns peer=%s q=%s from=%s rcode=%s", p.name, name, from, rcode)
			_, _ = pc.WriteTo(response, from)
		}
	}()
	return nil
}

func (p *wgPeer) close() {
	p.dev.Close()
}

// answerDNS builds the reply for one query: an A record for dnsAnswerName,
// NXDOMAIN for any other name. Non-A queries for the known name get an empty
// NOERROR answer.
func answerDNS(query []byte) (response []byte, name string, rcode dnsmessage.RCode, err error) {
	var parser dnsmessage.Parser
	header, err := parser.Start(query)
	if err != nil {
		return nil, "", 0, err
	}
	question, err := parser.Question()
	if err != nil {
		return nil, "", 0, err
	}
	name = strings.TrimSuffix(strings.ToLower(question.Name.String()), ".")
	known := name == dnsAnswerName
	rcode = dnsmessage.RCodeSuccess
	if !known {
		rcode = dnsmessage.RCodeNameError
	}
	builder := dnsmessage.NewBuilder(nil, dnsmessage.Header{
		ID:                 header.ID,
		Response:           true,
		Authoritative:      true,
		RecursionDesired:   header.RecursionDesired,
		RecursionAvailable: true,
		RCode:              rcode,
	})
	builder.EnableCompression()
	if err := builder.StartQuestions(); err != nil {
		return nil, name, rcode, err
	}
	if err := builder.Question(question); err != nil {
		return nil, name, rcode, err
	}
	if known && question.Type == dnsmessage.TypeA && question.Class == dnsmessage.ClassINET {
		if err := builder.StartAnswers(); err != nil {
			return nil, name, rcode, err
		}
		err := builder.AResource(dnsmessage.ResourceHeader{
			Name:  question.Name,
			Type:  dnsmessage.TypeA,
			Class: dnsmessage.ClassINET,
			TTL:   60,
		}, dnsmessage.AResource{A: labRoutedAddr.As4()})
		if err != nil {
			return nil, name, rcode, err
		}
	}
	response, err = builder.Finish()
	return response, name, rcode, err
}

// fixturePeers are the nodes the map advertises with real keys: the early
// and full scenarios' target, the subnet router that fronts the routed
// address, and the split-DNS resolver.
type fixturePeers struct {
	apiGateway *wgPeer
	labGW      *wgPeer
	labDNS     *wgPeer
}

// startFixturePeers starts the three peers; impair (nil for the clean path)
// is shared by their binds so its counters cover the whole relay.
func startFixturePeers(region func() *tailcfg.DERPRegion, impair *impairment, logf logger.Logf) (*fixturePeers, error) {
	apiGateway, err := startImpairedWGPeer("api-gateway", []netip.Addr{apiGatewayAddr}, region, impair, logf)
	if err != nil {
		return nil, err
	}
	labGW, err := startImpairedWGPeer("lab-gw", []netip.Addr{labGWAddr, labRoutedAddr}, region, impair, logf)
	if err != nil {
		apiGateway.close()
		return nil, err
	}
	labDNS, err := startImpairedWGPeer("lab-dns", []netip.Addr{labDNSAddr}, region, impair, logf)
	if err != nil {
		apiGateway.close()
		labGW.close()
		return nil, err
	}
	if err := labDNS.serveDNS(labDNSAddr); err != nil {
		apiGateway.close()
		labGW.close()
		labDNS.close()
		return nil, err
	}
	return &fixturePeers{apiGateway: apiGateway, labGW: labGW, labDNS: labDNS}, nil
}

func (p *fixturePeers) all() []*wgPeer {
	return []*wgPeer{p.apiGateway, p.labGW, p.labDNS}
}

// setReader points every fixture peer at the reader's node key.
func (p *fixturePeers) setReader(reader key.NodePublic) error {
	for _, peer := range p.all() {
		if err := peer.setReader(reader, readerPrefix); err != nil {
			return fmt.Errorf("%s: %w", peer.name, err)
		}
	}
	return nil
}

func (p *fixturePeers) close() {
	for _, peer := range p.all() {
		peer.close()
	}
}

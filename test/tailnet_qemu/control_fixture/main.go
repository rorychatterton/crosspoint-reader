package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"time"

	"golang.org/x/net/http2"
	"tailscale.com/control/controlhttp/controlhttpserver"
	"tailscale.com/derp/derpserver"
	"tailscale.com/types/key"
)

const (
	listenAddress = "127.0.0.1:18080"
	// The firmware is built with -DML_DERP_HOST="10.0.2.100" -DML_DERP_PORT=18443;
	// run_integration.sh forwards that guest address here.
	derpListenHost    = "127.0.0.1"
	derpListenPort    = 18443
	derpListenAddress = "127.0.0.1:18443"
	authKey           = "qemu-integration-authkey"
	privateKeyHex     = "privkey:000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"

	// Placeholder node keys for unit tests that build maps without starting
	// the WireGuard peers; main() replaces them with the peers' real keys.
	placeholderTargetKey  = "nodekey:1111111111111111111111111111111111111111111111111111111111111111"
	placeholderGatewayKey = "nodekey:3333333333333333333333333333333333333333333333333333333333333333"
	placeholderDNSKey     = "nodekey:5555555555555555555555555555555555555555555555555555555555555555"
)

// selfSignedCert mints a throwaway ECDSA certificate for the DERP listener.
// MicroLink connects with MBEDTLS_SSL_VERIFY_NONE, so nothing checks it.
func selfSignedCert() (tls.Certificate, error) {
	private, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, err
	}
	template := x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "derp.qemu.test"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		DNSNames:     []string{"derp.qemu.test"},
		IPAddresses:  []net.IP{net.ParseIP("10.0.2.100"), net.ParseIP("127.0.0.1")},
	}
	der, err := x509.CreateCertificate(rand.Reader, &template, &template, &private.PublicKey, private)
	if err != nil {
		return tls.Certificate{}, err
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: private}, nil
}

// startDerp serves a real Tailscale DERP relay over TLS so the firmware's
// relay stage (TLS, HTTP upgrade, ServerKey/ClientInfo handshake, relayed
// WireGuard initiation) runs without internet access.
func startDerp() (*derpserver.Server, error) {
	cert, err := selfSignedCert()
	if err != nil {
		return nil, err
	}
	server := derpserver.New(key.NewNode(), log.Printf)
	handler := derpserver.Handler(server)
	mux := http.NewServeMux()
	mux.HandleFunc("/derp", func(w http.ResponseWriter, r *http.Request) {
		log.Printf("DERP_FIXTURE upgrade remote=%s upgrade=%q", r.RemoteAddr, r.Header.Get("Upgrade"))
		handler.ServeHTTP(w, r)
	})
	listener, err := tls.Listen("tcp", derpListenAddress, &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS12,
	})
	if err != nil {
		return nil, err
	}
	log.Printf("DERP_FIXTURE listening=%s public_key=%s", derpListenAddress, server.PublicKey().UntypedHexString())
	go func() {
		log.Fatal(http.Serve(listener, mux))
	}()
	return server, nil
}

type fixture struct {
	noisePrivate key.MachinePrivate
	delayMapTail bool
	// Node keys the map advertises for the target, lab-gw and lab-dns.
	targetKey  string
	gatewayKey string
	dnsKey     string
	// peers, when set, are configured with each reader's node key at map
	// time so the DERP-relayed handshake completes.
	peers *fixturePeers
}

func newFixture() *fixture {
	var private key.MachinePrivate
	if err := private.UnmarshalText([]byte(privateKeyHex)); err != nil {
		panic(err)
	}
	return &fixture{
		noisePrivate: private,
		targetKey:    placeholderTargetKey,
		gatewayKey:   placeholderGatewayKey,
		dnsKey:       placeholderDNSKey,
	}
}

// usePeers advertises the running peers' keys in the map and configures
// them with each reader that fetches it.
func (f *fixture) usePeers(peers *fixturePeers) {
	f.peers = peers
	f.targetKey = peers.apiGateway.nodeKey()
	f.gatewayKey = peers.labGW.nodeKey()
	f.dnsKey = peers.labDNS.nodeKey()
}

// configureReader parses the reader's node key from a request and makes it
// the WireGuard peer of every fixture node.
func (f *fixture) configureReader(nodeKey string) {
	if f.peers == nil {
		return
	}
	var reader key.NodePublic
	if err := reader.UnmarshalText([]byte(nodeKey)); err != nil {
		log.Printf("WG_FIXTURE reader_key_invalid node=%q err=%v", nodeKey, err)
		return
	}
	if err := f.peers.setReader(reader); err != nil {
		log.Printf("WG_FIXTURE reader_configure_failed node=%s err=%v", reader.String(), err)
		return
	}
	log.Printf("WG_FIXTURE reader_configured node=%s allowed=%s", reader.String(), readerPrefix)
}

func (f *fixture) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/ts2021" {
		http.NotFound(w, r)
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 90*time.Second)
	defer cancel()
	conn, err := controlhttpserver.AcceptHTTP(ctx, w, r, f.noisePrivate, nil)
	if err != nil {
		log.Printf("CONTROL_FIXTURE noise_error=%q", err)
		return
	}
	defer conn.Close()
	log.Printf("CONTROL_FIXTURE noise_ok protocol=%d peer=%s", conn.ProtocolVersion(), conn.Peer())

	handler := http.NewServeMux()
	handler.HandleFunc("/machine/register", f.register)
	handler.HandleFunc("/machine/map", f.peerMap)
	server := http2.Server{}
	server.ServeConn(conn, &http2.ServeConnOpts{
		Context: ctx,
		BaseConfig: &http.Server{
			Handler: handler,
		},
	})
}

func (f *fixture) register(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(io.LimitReader(r.Body, 64<<10))
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	var request struct {
		Version int    `json:"Version"`
		NodeKey string `json:"NodeKey"`
		Auth    struct {
			AuthKey string `json:"AuthKey"`
		} `json:"Auth"`
	}
	if err := json.Unmarshal(body, &request); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	if request.Version != 131 || request.NodeKey == "" || request.Auth.AuthKey != authKey {
		log.Printf("CONTROL_FIXTURE register_rejected version=%d node=%q auth_match=%t",
			request.Version, request.NodeKey, request.Auth.AuthKey == authKey)
		http.Error(w, "invalid registration", http.StatusForbidden)
		return
	}
	log.Printf("CONTROL_FIXTURE register_ok bytes=%d", len(body))
	w.Header().Set("Content-Type", "application/json")
	_, _ = io.WriteString(w, "{}")
}

func (f *fixture) peerMap(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(io.LimitReader(r.Body, 64<<10))
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	var request struct {
		Version  int    `json:"Version"`
		NodeKey  string `json:"NodeKey"`
		Stream   bool   `json:"Stream"`
		Hostinfo struct {
			Hostname string `json:"Hostname"`
		} `json:"Hostinfo"`
	}
	if err := json.Unmarshal(body, &request); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	if request.Version != 131 || request.NodeKey == "" || request.Stream {
		http.Error(w, "invalid map request", http.StatusBadRequest)
		return
	}
	f.configureReader(request.NodeKey)
	if request.Hostinfo.Hostname == "crosspoint-qemu-malformed" {
		encoded := []byte(`{"Node":tx}`)
		var size [4]byte
		binary.LittleEndian.PutUint32(size[:], uint32(len(encoded)))
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write(size[:])
		_, _ = w.Write(encoded)
		log.Printf("CONTROL_FIXTURE map_malformed scenario=%s", request.Hostinfo.Hostname)
		return
	}

	target := map[string]any{
		"Name":       "api-gateway.integration.test.ts.net.",
		"Key":        f.targetKey,
		"DiscoKey":   "discokey:2222222222222222222222222222222222222222222222222222222222222222",
		"Addresses":  []string{"100.64.0.42/32"},
		"AllowedIPs": []string{"100.64.0.42/32"},
		"HomeDERP":   9,
		"Endpoints":  []string{"10.0.2.2:41641"},
		"Online":     true,
	}
	hostname := request.Hostinfo.Hostname
	targetLast := hostname == "crosspoint-qemu-full" || hostname == "crosspoint-qemu-stalled"
	peers := make([]any, 0, 403)
	// Resolver-shaped maps are the routed map plus a lab-dns node. The
	// restart scenario fetches the same map twice (resolver alone, then the
	// target with the resolver as secondary). The late variant lists lab-dns
	// after lab-gw so the filter has to keep streaming past the target; the
	// absent variant omits lab-dns while the firmware still asks for it.
	resolverFirst := hostname == "crosspoint-qemu-resolver" || hostname == "crosspoint-qemu-restart"
	resolverLast := hostname == "crosspoint-qemu-resolver-late"
	resolverAbsent := hostname == "crosspoint-qemu-resolver-absent"
	routed := hostname == "crosspoint-qemu-routed" || resolverFirst || resolverLast || resolverAbsent
	omitTarget := hostname == "crosspoint-qemu-missing" || routed
	// A tailnet DNS resolver node. Listed first, the filter must hold it while
	// it streams the rest of the map looking for the target.
	labDNS := map[string]any{
		"Name":       "lab-dns.integration.test.ts.net.",
		"Key":        f.dnsKey,
		"DiscoKey":   "discokey:6666666666666666666666666666666666666666666666666666666666666666",
		"Addresses":  []string{"100.64.0.53/32"},
		"AllowedIPs": []string{"100.64.0.53/32"},
		"HomeDERP":   9,
		"Endpoints":  []string{"10.0.2.2:41641"},
		"Online":     true,
	}
	if resolverFirst {
		peers = append(peers, labDNS)
	}
	if !targetLast && !omitTarget {
		peers = append(peers, target)
	}
	for i := 0; i < 400; i++ {
		address := fmt.Sprintf("100.65.%d.%d/32", i/250, i%250+1)
		peers = append(peers, map[string]any{
			"Name":       fmt.Sprintf("peer-%03d.integration.test.ts.net.", i),
			"Key":        fmt.Sprintf("nodekey:%064x", i+1),
			"DiscoKey":   fmt.Sprintf("discokey:%064x", i+1001),
			"Addresses":  []string{address},
			"AllowedIPs": []string{address},
			"HomeDERP":   i%20 + 1,
			"Endpoints":  []string{fmt.Sprintf("192.0.2.%d:41641", i%250+1)},
			"Online":     true,
		})
	}
	if targetLast && !omitTarget {
		peers = append(peers, target)
	}
	if routed {
		// Subnet router / VIP service shape: the target address appears only in
		// AllowedIPs and PrimaryRoutes, never in Addresses.
		peers = append(peers, map[string]any{
			"Name":          "lab-gw.integration.test.ts.net.",
			"Key":           f.gatewayKey,
			"DiscoKey":      "discokey:4444444444444444444444444444444444444444444444444444444444444444",
			"Addresses":     []string{"100.64.0.77/32"},
			"AllowedIPs":    []string{"100.64.0.77/32", "100.70.0.42/32"},
			"PrimaryRoutes": []string{"100.70.0.42/32"},
			"HomeDERP":      9,
			"Endpoints":     []string{"10.0.2.2:41641"},
			"Online":        true,
		})
	}
	if resolverLast {
		peers = append(peers, labDNS)
	}
	response := map[string]any{
		"Node": map[string]any{
			"Addresses": []string{"100.64.0.1/32"},
			"HomeDERP":  9,
			"Key":       request.NodeKey,
		},
		"Peers": peers,
	}
	encoded, err := json.Marshal(response)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	var size [4]byte
	binary.LittleEndian.PutUint32(size[:], uint32(len(encoded)))
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(size[:])
	delayTail := f.delayMapTail && (hostname == "crosspoint-qemu-early" || hostname == "crosspoint-qemu-stalled")
	if delayTail && len(encoded) > 4096 {
		_, _ = w.Write(encoded[:4096])
		if flusher, ok := w.(http.Flusher); ok {
			flusher.Flush()
		}
		log.Printf("CONTROL_FIXTURE map_prefix_sent scenario=%s bytes=4100 remaining=%d", hostname, len(encoded)-4096)
		time.Sleep(20 * time.Second)
		_, _ = w.Write(encoded[4096:])
	} else {
		_, _ = w.Write(encoded)
	}
	log.Printf("CONTROL_FIXTURE map_ok scenario=%s request_bytes=%d response_bytes=%d", hostname, len(body),
		len(encoded))
}

// pipe copies guest bytes from in to a fresh TCP connection to target and the
// replies back to out, returning when either side closes. QEMU's
// guestfwd=...-cmd: form runs one such process per guest connection, which
// the chardev form (guestfwd=...-tcp:host:port) cannot do: that opens a
// single host connection at QEMU start and multiplexes every guest
// connection onto it, so a second session's control or DERP connection lands
// inside the first one's stream. stdin, stdout and stderr are all the same
// socket in this mode, so nothing may be logged.
func pipe(in io.Reader, out io.Writer, target string) error {
	conn, err := net.Dial("tcp", target)
	if err != nil {
		return err
	}
	defer conn.Close()
	inDone := make(chan struct{})
	go func() {
		_, _ = io.Copy(conn, in)
		if tcp, ok := conn.(*net.TCPConn); ok {
			_ = tcp.CloseWrite()
		}
		close(inDone)
	}()
	outDone := make(chan struct{})
	go func() {
		_, _ = io.Copy(out, conn)
		close(outDone)
	}()
	select {
	case <-outDone:
	case <-inDone:
		// The guest has finished sending: relay whatever the target still has
		// to say, but do not outlive a target that never closes.
		_ = conn.SetReadDeadline(time.Now().Add(30 * time.Second))
		<-outDone
	}
	return nil
}

func main() {
	printKey := flag.Bool("print-key", false, "print the deterministic Noise public key and exit")
	pipeTarget := flag.String("pipe", "", "forward stdin/stdout to this TCP address and exit (QEMU guestfwd cmd: helper)")
	dropPct := flag.Int("drop-pct", -1, "drop this percentage of relayed WireGuard frames (default $"+envDropPct+")")
	dupPct := flag.Int("dup-pct", -1, "duplicate this percentage of relayed WireGuard frames (default $"+envDupPct+")")
	reorderPct := flag.Int("reorder-pct", -1,
		"hold this percentage of outbound frames back one slot (default $"+envReorderPct+")")
	flag.Parse()
	impair, err := impairmentFromEnv()
	if err != nil {
		log.Fatalf("WG_FIXTURE impairment: %v", err)
	}
	if *dropPct >= 0 || *dupPct >= 0 || *reorderPct >= 0 {
		if impair == nil {
			impair = newImpairment(0, 0, 0, 1)
		}
		if *dropPct >= 0 {
			impair.dropPct = *dropPct
		}
		if *dupPct >= 0 {
			impair.dupPct = *dupPct
		}
		if *reorderPct >= 0 {
			impair.reorderPct = *reorderPct
		}
		if impair.dropPct == 0 && impair.dupPct == 0 && impair.reorderPct == 0 {
			impair = nil
		}
	}
	if *pipeTarget != "" {
		if err := pipe(os.Stdin, os.Stdout, *pipeTarget); err != nil {
			os.Exit(1)
		}
		return
	}
	server := newFixture()
	if *printKey {
		fmt.Println(server.noisePrivate.Public().UntypedHexString())
		return
	}
	server.delayMapTail = true
	derp, err := startDerp()
	if err != nil {
		log.Fatalf("DERP_FIXTURE start failed: %v", err)
	}
	defer derp.Close()
	if impair != nil {
		log.Printf("WG_FIXTURE impairment %s", impair)
	}
	peers, err := startFixturePeers(fixtureDERPRegion(derpListenHost, derpListenPort), impair, log.Printf)
	if err != nil {
		log.Fatalf("WG_FIXTURE start failed: %v", err)
	}
	defer peers.close()
	server.usePeers(peers)
	log.Printf("CONTROL_FIXTURE listening=%s public_key=%s", listenAddress,
		server.noisePrivate.Public().UntypedHexString())
	log.Fatal(http.ListenAndServe(listenAddress, server))
}

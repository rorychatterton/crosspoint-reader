package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestControlPublicKeyIsStable(t *testing.T) {
	const want = "8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f"
	if got := newFixture().noisePrivate.Public().UntypedHexString(); got != want {
		t.Fatalf("public key = %s, want %s", got, want)
	}
}

func TestRegisterAcceptsHarnessRequest(t *testing.T) {
	body := []byte(`{"Version":131,"NodeKey":"nodekey:01","Auth":{"AuthKey":"qemu-integration-authkey"}}`)
	request := httptest.NewRequest(http.MethodPost, "/machine/register", bytes.NewReader(body))
	response := httptest.NewRecorder()
	newFixture().register(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %q", response.Code, response.Body.String())
	}
}

func TestMapContainsTargetPeerAndLengthPrefix(t *testing.T) {
	body := []byte(`{"Version":131,"NodeKey":"nodekey:01","Stream":false}`)
	request := httptest.NewRequest(http.MethodPost, "/machine/map", bytes.NewReader(body))
	response := httptest.NewRecorder()
	newFixture().peerMap(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %q", response.Code, response.Body.String())
	}
	encoded := response.Body.Bytes()
	if len(encoded) < 5 {
		t.Fatalf("response has %d bytes", len(encoded))
	}
	if got, want := binary.LittleEndian.Uint32(encoded[:4]), uint32(len(encoded)-4); got != want {
		t.Fatalf("length prefix = %d, want %d", got, want)
	}
	var peerMap struct {
		Peers []struct {
			Name      string   `json:"Name"`
			Addresses []string `json:"Addresses"`
		} `json:"Peers"`
	}
	if err := json.Unmarshal(encoded[4:], &peerMap); err != nil {
		t.Fatal(err)
	}
	if len(peerMap.Peers) != 401 {
		t.Fatalf("peer count = %d, want 401", len(peerMap.Peers))
	}
	target := peerMap.Peers[0]
	if target.Name != "api-gateway.integration.test.ts.net." || len(target.Addresses) != 1 ||
		target.Addresses[0] != "100.64.0.42/32" {
		t.Fatalf("unexpected target peer: %+v", target)
	}
}

func TestFullMapScenarioPlacesTargetLast(t *testing.T) {
	body := []byte(`{"Version":131,"NodeKey":"nodekey:01","Stream":false,"Hostinfo":{"Hostname":"crosspoint-qemu-full"}}`)
	request := httptest.NewRequest(http.MethodPost, "/machine/map", bytes.NewReader(body))
	response := httptest.NewRecorder()
	newFixture().peerMap(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %q", response.Code, response.Body.String())
	}
	var peerMap struct {
		Peers []struct {
			Name string `json:"Name"`
		} `json:"Peers"`
	}
	if err := json.Unmarshal(response.Body.Bytes()[4:], &peerMap); err != nil {
		t.Fatal(err)
	}
	if got := peerMap.Peers[len(peerMap.Peers)-1].Name; got != "api-gateway.integration.test.ts.net." {
		t.Fatalf("last peer = %q", got)
	}
}

// Each pipe invocation must open its own connection to the target and end
// when the guest side closes, so consecutive guest sessions never share a
// host connection.
func TestPipeOpensOneConnectionPerInvocation(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	accepted := make(chan struct{}, 4)
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			accepted <- struct{}{}
			go func() {
				defer conn.Close()
				buf := make([]byte, 64)
				n, _ := conn.Read(buf)
				_, _ = conn.Write(append([]byte("echo:"), buf[:n]...))
			}()
		}
	}()
	for i := 0; i < 2; i++ {
		var out bytes.Buffer
		if err := pipe(strings.NewReader("hello"), &out, listener.Addr().String()); err != nil {
			t.Fatal(err)
		}
		if out.String() != "echo:hello" {
			t.Fatalf("pipe %d returned %q", i, out.String())
		}
	}
	if got := len(accepted); got != 2 {
		t.Fatalf("target accepted %d connections, want 2", got)
	}
}

type mapPeer struct {
	Name       string   `json:"Name"`
	Addresses  []string `json:"Addresses"`
	AllowedIPs []string `json:"AllowedIPs"`
}

// fetchPeers returns the peer list the fixture serves for a device hostname.
func fetchPeers(t *testing.T, hostname string) []mapPeer {
	t.Helper()
	body := []byte(`{"Version":131,"NodeKey":"nodekey:01","Stream":false,"Hostinfo":{"Hostname":"` + hostname + `"}}`)
	request := httptest.NewRequest(http.MethodPost, "/machine/map", bytes.NewReader(body))
	response := httptest.NewRecorder()
	newFixture().peerMap(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %q", response.Code, response.Body.String())
	}
	var peerMap struct {
		Peers []mapPeer `json:"Peers"`
	}
	if err := json.Unmarshal(response.Body.Bytes()[4:], &peerMap); err != nil {
		t.Fatal(err)
	}
	return peerMap.Peers
}

func indexOfPeer(peers []mapPeer, name string) int {
	for i, peer := range peers {
		if peer.Name == name {
			return i
		}
	}
	return -1
}

const (
	labDNSName = "lab-dns.integration.test.ts.net."
	labGWName  = "lab-gw.integration.test.ts.net."
)

func TestResolverScenariosListResolverFirstAndGatewayLast(t *testing.T) {
	for _, hostname := range []string{"crosspoint-qemu-resolver", "crosspoint-qemu-restart"} {
		peers := fetchPeers(t, hostname)
		if len(peers) != 402 {
			t.Fatalf("%s: peer count = %d, want 402", hostname, len(peers))
		}
		if peers[0].Name != labDNSName {
			t.Fatalf("%s: first peer = %q, want %q", hostname, peers[0].Name, labDNSName)
		}
		if got := peers[len(peers)-1].Name; got != labGWName {
			t.Fatalf("%s: last peer = %q, want %q", hostname, got, labGWName)
		}
		if indexOfPeer(peers, "api-gateway.integration.test.ts.net.") != -1 {
			t.Fatalf("%s: api-gateway must not be in the routed map", hostname)
		}
	}
}

func TestResolverLateScenarioListsResolverAfterGateway(t *testing.T) {
	peers := fetchPeers(t, "crosspoint-qemu-resolver-late")
	if len(peers) != 402 {
		t.Fatalf("peer count = %d, want 402", len(peers))
	}
	gw := indexOfPeer(peers, labGWName)
	dns := indexOfPeer(peers, labDNSName)
	if gw != len(peers)-2 || dns != len(peers)-1 {
		t.Fatalf("lab-gw at %d and lab-dns at %d, want the last two positions in that order", gw, dns)
	}
}

func TestResolverAbsentScenarioOmitsResolver(t *testing.T) {
	peers := fetchPeers(t, "crosspoint-qemu-resolver-absent")
	if len(peers) != 401 {
		t.Fatalf("peer count = %d, want 401", len(peers))
	}
	if indexOfPeer(peers, labDNSName) != -1 {
		t.Fatal("lab-dns must be absent")
	}
	gw := peers[len(peers)-1]
	if gw.Name != labGWName || len(gw.AllowedIPs) != 2 || gw.AllowedIPs[1] != "100.70.0.42/32" {
		t.Fatalf("unexpected last peer: %+v", gw)
	}
}

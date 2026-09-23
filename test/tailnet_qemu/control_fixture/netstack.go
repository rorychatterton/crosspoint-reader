package main

import (
	"context"
	"fmt"
	"net"
	"net/netip"
	"os"
	"sync"
	"syscall"

	"github.com/tailscale/wireguard-go/tun"
	"gvisor.dev/gvisor/pkg/buffer"
	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/adapters/gonet"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/link/channel"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv4"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv6"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/icmp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/udp"
)

// netTun is a wireguard-go tun.Device backed by a gVisor stack, after
// wireguard-go's tun/netstack, which does not build against the gVisor
// version tailscale.com pins. Packets wireguard-go writes are injected into
// the stack; packets the stack emits are read by wireguard-go.
type netTun struct {
	ep        *channel.Endpoint
	stack     *stack.Stack
	events    chan tun.Event
	outbound  chan *buffer.View
	done      chan struct{}
	closeOnce sync.Once
	mtu       int
}

// stackNet dials and listens inside a netTun's stack.
type stackNet struct {
	stack *stack.Stack
}

const nicID = 1

func createNetTUN(localAddresses []netip.Addr, mtu int) (tun.Device, *stackNet, error) {
	opts := stack.Options{
		NetworkProtocols:   []stack.NetworkProtocolFactory{ipv4.NewProtocol, ipv6.NewProtocol},
		TransportProtocols: []stack.TransportProtocolFactory{tcp.NewProtocol, udp.NewProtocol, icmp.NewProtocol4, icmp.NewProtocol6},
		HandleLocal:        true,
	}
	dev := &netTun{
		ep:       channel.New(1024, uint32(mtu), ""),
		stack:    stack.New(opts),
		events:   make(chan tun.Event, 10),
		outbound: make(chan *buffer.View, 1024),
		done:     make(chan struct{}),
		mtu:      mtu,
	}
	sack := tcpip.TCPSACKEnabled(true)
	if err := dev.stack.SetTransportProtocolOption(tcp.ProtocolNumber, &sack); err != nil {
		return nil, nil, fmt.Errorf("enable TCP SACK: %v", err)
	}
	dev.ep.AddNotify(dev)
	if err := dev.stack.CreateNIC(nicID, dev.ep); err != nil {
		return nil, nil, fmt.Errorf("CreateNIC: %v", err)
	}
	hasV4, hasV6 := false, false
	for _, ip := range localAddresses {
		protoNumber := ipv4.ProtocolNumber
		if ip.Is6() {
			protoNumber = ipv6.ProtocolNumber
		}
		protoAddr := tcpip.ProtocolAddress{
			Protocol:          protoNumber,
			AddressWithPrefix: tcpip.AddrFromSlice(ip.AsSlice()).WithPrefix(),
		}
		if err := dev.stack.AddProtocolAddress(nicID, protoAddr, stack.AddressProperties{}); err != nil {
			return nil, nil, fmt.Errorf("AddProtocolAddress(%v): %v", ip, err)
		}
		if ip.Is4() {
			hasV4 = true
		} else {
			hasV6 = true
		}
	}
	if hasV4 {
		dev.stack.AddRoute(tcpip.Route{Destination: header.IPv4EmptySubnet, NIC: nicID})
	}
	if hasV6 {
		dev.stack.AddRoute(tcpip.Route{Destination: header.IPv6EmptySubnet, NIC: nicID})
	}
	dev.events <- tun.EventUp
	return dev, &stackNet{stack: dev.stack}, nil
}

func (t *netTun) Name() (string, error)    { return "go", nil }
func (t *netTun) File() *os.File           { return nil }
func (t *netTun) Events() <-chan tun.Event { return t.events }
func (t *netTun) MTU() (int, error)        { return t.mtu, nil }
func (t *netTun) BatchSize() int           { return 1 }

// Read hands one stack-emitted packet to wireguard-go.
func (t *netTun) Read(bufs [][]byte, sizes []int, offset int) (int, error) {
	var view *buffer.View
	select {
	case view = <-t.outbound:
	case <-t.done:
		return 0, os.ErrClosed
	}
	n, err := view.Read(bufs[0][offset:])
	view.Release()
	if err != nil {
		return 0, err
	}
	sizes[0] = n
	return 1, nil
}

// Write injects decrypted packets from wireguard-go into the stack.
func (t *netTun) Write(bufs [][]byte, offset int) (int, error) {
	for _, buf := range bufs {
		packet := buf[offset:]
		if len(packet) == 0 {
			continue
		}
		pkb := stack.NewPacketBuffer(stack.PacketBufferOptions{Payload: buffer.MakeWithData(packet)})
		switch packet[0] >> 4 {
		case 4:
			t.ep.InjectInbound(header.IPv4ProtocolNumber, pkb)
		case 6:
			t.ep.InjectInbound(header.IPv6ProtocolNumber, pkb)
		default:
			pkb.DecRef()
			return 0, syscall.EAFNOSUPPORT
		}
		pkb.DecRef()
	}
	return len(bufs), nil
}

// WriteNotify is the channel endpoint's callback for a newly queued
// outbound packet.
func (t *netTun) WriteNotify() {
	pkt := t.ep.Read()
	if pkt == nil {
		return
	}
	view := pkt.ToView()
	pkt.DecRef()
	select {
	case t.outbound <- view:
	case <-t.done:
		view.Release()
	}
}

// Close stops the stack and unblocks Read; the outbound channel is never
// closed because the stack may still be notifying.
func (t *netTun) Close() error {
	t.closeOnce.Do(func() {
		close(t.done)
		t.stack.RemoveNIC(nicID)
		t.ep.Close()
		close(t.events)
	})
	return nil
}

func fullAddr(addr netip.AddrPort) (tcpip.FullAddress, tcpip.NetworkProtocolNumber) {
	proto := ipv4.ProtocolNumber
	if addr.Addr().Is6() {
		proto = ipv6.ProtocolNumber
	}
	full := tcpip.FullAddress{NIC: nicID, Port: addr.Port()}
	if addr.Addr().IsValid() {
		full.Addr = tcpip.AddrFromSlice(addr.Addr().AsSlice())
	}
	return full, proto
}

// ListenTCP listens on every local address when addr's IP is unspecified.
func (n *stackNet) ListenTCP(addr netip.AddrPort) (net.Listener, error) {
	full, proto := fullAddr(addr)
	return gonet.ListenTCP(n.stack, full, proto)
}

func (n *stackNet) ListenUDP(addr netip.AddrPort) (net.PacketConn, error) {
	full, proto := fullAddr(addr)
	return gonet.DialUDP(n.stack, &full, nil, proto)
}

func (n *stackNet) DialContextTCP(ctx context.Context, addr netip.AddrPort) (net.Conn, error) {
	full, proto := fullAddr(addr)
	return gonet.DialContextTCP(ctx, n.stack, full, proto)
}

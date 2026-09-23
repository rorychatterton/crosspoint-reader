package main

import (
	"fmt"
	"math/rand"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"tailscale.com/types/key"
)

// impairment drops, duplicates and reorders the DERP frames a derpBind
// relays, so the reader's TCP has to recover the way it must on a lossy
// real-world path. Percentages are per frame. Drop and duplicate apply in
// both directions; reorder holds an outbound frame back until the next one
// has gone (or holdFor elapses), which is the only reorder shape a single
// relay hop can produce. The generator is seeded so a run is reproducible.
type impairment struct {
	dropPct    int
	dupPct     int
	reorderPct int
	seed       int64
	holdFor    time.Duration

	mu    sync.Mutex
	rng   *rand.Rand
	held  *heldFrame
	timer *time.Timer

	dropped    atomic.Uint64
	duplicated atomic.Uint64
	reordered  atomic.Uint64
}

type heldFrame struct {
	dst  key.NodePublic
	data []byte
	send func(key.NodePublic, []byte) error
}

const (
	envDropPct    = "FIXTURE_DROP_PCT"
	envDupPct     = "FIXTURE_DUP_PCT"
	envReorderPct = "FIXTURE_REORDER_PCT"
	envImpairSeed = "FIXTURE_IMPAIR_SEED"

	// defaultHoldFor bounds how long a reordered frame waits for a successor;
	// a lone handshake packet must not sit in the hold indefinitely.
	defaultHoldFor = 30 * time.Millisecond
)

func newImpairment(dropPct, dupPct, reorderPct int, seed int64) *impairment {
	return &impairment{
		dropPct:    dropPct,
		dupPct:     dupPct,
		reorderPct: reorderPct,
		seed:       seed,
		holdFor:    defaultHoldFor,
		rng:        rand.New(rand.NewSource(seed)),
	}
}

func envPct(name string) (int, error) {
	value := os.Getenv(name)
	if value == "" {
		return 0, nil
	}
	pct, err := strconv.Atoi(value)
	if err != nil || pct < 0 || pct > 100 {
		return 0, fmt.Errorf("%s=%q: want an integer percentage 0-100", name, value)
	}
	return pct, nil
}

// impairmentFromEnv reads FIXTURE_DROP_PCT, FIXTURE_DUP_PCT,
// FIXTURE_REORDER_PCT and FIXTURE_IMPAIR_SEED. It returns nil when every
// percentage is zero or unset, so the clean scenarios pay nothing.
func impairmentFromEnv() (*impairment, error) {
	drop, err := envPct(envDropPct)
	if err != nil {
		return nil, err
	}
	dup, err := envPct(envDupPct)
	if err != nil {
		return nil, err
	}
	reorder, err := envPct(envReorderPct)
	if err != nil {
		return nil, err
	}
	if drop == 0 && dup == 0 && reorder == 0 {
		return nil, nil
	}
	var seed int64 = 1
	if value := os.Getenv(envImpairSeed); value != "" {
		seed, err = strconv.ParseInt(value, 10, 64)
		if err != nil {
			return nil, fmt.Errorf("%s=%q: want an integer", envImpairSeed, value)
		}
	}
	return newImpairment(drop, dup, reorder, seed), nil
}

func (im *impairment) String() string {
	return fmt.Sprintf("drop_pct=%d dup_pct=%d reorder_pct=%d seed=%d", im.dropPct, im.dupPct, im.reorderPct, im.seed)
}

// stats renders the counters for the fixture log.
func (im *impairment) stats() string {
	return fmt.Sprintf("dropped=%d duplicated=%d reordered=%d", im.dropped.Load(), im.duplicated.Load(),
		im.reordered.Load())
}

// roll is true with probability pct/100. Callers hold im.mu.
func (im *impairment) roll(pct int) bool {
	return pct > 0 && im.rng.Intn(100) < pct
}

// send relays one outbound frame through the impairment: dropped, held for
// reordering, sent once, or sent twice. A frame held from an earlier call is
// released after this one.
func (im *impairment) send(dst key.NodePublic, data []byte, send func(key.NodePublic, []byte) error) error {
	im.mu.Lock()
	if im.roll(im.dropPct) {
		im.mu.Unlock()
		im.dropped.Add(1)
		return nil
	}
	released := im.takeHeldLocked()
	hold := released == nil && im.roll(im.reorderPct)
	dup := !hold && im.roll(im.dupPct)
	if hold {
		im.held = &heldFrame{dst: dst, data: append([]byte(nil), data...), send: send}
		im.timer = time.AfterFunc(im.holdFor, im.flush)
		im.mu.Unlock()
		return nil
	}
	im.mu.Unlock()
	if err := send(dst, data); err != nil {
		return err
	}
	if dup {
		im.duplicated.Add(1)
		if err := send(dst, data); err != nil {
			return err
		}
	}
	if released != nil {
		im.reordered.Add(1)
		return released.send(released.dst, released.data)
	}
	return nil
}

// takeHeldLocked detaches the held frame and its flush timer. Callers hold
// im.mu.
func (im *impairment) takeHeldLocked() *heldFrame {
	held := im.held
	if held == nil {
		return nil
	}
	im.held = nil
	if im.timer != nil {
		im.timer.Stop()
		im.timer = nil
	}
	return held
}

// flush releases a held frame whose successor never came, in order.
func (im *impairment) flush() {
	im.mu.Lock()
	held := im.takeHeldLocked()
	im.mu.Unlock()
	if held != nil {
		_ = held.send(held.dst, held.data)
	}
}

// dropIncoming reports whether one received frame should be discarded.
func (im *impairment) dropIncoming() bool {
	im.mu.Lock()
	drop := im.roll(im.dropPct)
	im.mu.Unlock()
	if drop {
		im.dropped.Add(1)
	}
	return drop
}

// duplicateIncoming reports whether one received frame should be delivered
// twice.
func (im *impairment) duplicateIncoming() bool {
	im.mu.Lock()
	dup := im.roll(im.dupPct)
	im.mu.Unlock()
	if dup {
		im.duplicated.Add(1)
	}
	return dup
}

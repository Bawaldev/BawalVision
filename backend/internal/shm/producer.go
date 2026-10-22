package shm

import (
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

const (
	shmPath      = "/dev/shm/bawal_frame_shm"
	sockPath     = "/dev/shm/bawal_shm.sock"
	magicBAWL    = 0x4241574C
	numSlots     = 4
	slotCapacity = 1024 * 1024 // 1 MB
	slotHeaderSz = 64
	slotStride   = slotHeaderSz + slotCapacity
	globalHdrSz  = 64
	totalShmSize = globalHdrSz + (numSlots * slotStride)
)

// ShmProducer writes the newest JPEG frame into a POSIX shared-memory ring.
// The C++ vision service maps the same file read-only, so the HTTP ingress path
// only pays for a single memory copy instead of an extra IPC serialization.
type ShmProducer struct {
	file     *os.File
	mmapData []byte

	mu         sync.Mutex
	writeSeq   uint64
	notifyConn net.Conn
}

func NewShmProducer() (*ShmProducer, error) {
	f, err := os.OpenFile(shmPath, os.O_RDWR|os.O_CREATE, 0666)
	if err != nil {
		return nil, fmt.Errorf("failed open shm: %w", err)
	}

	if err := f.Truncate(int64(totalShmSize)); err != nil {
		_ = f.Close()
		return nil, fmt.Errorf("failed truncate shm: %w", err)
	}
	// The vision container runs as a different (non-root) user. Make the shared
	// mapping world-readable/writable so both sides can map it.
	_ = os.Chmod(shmPath, 0666)

	data, err := syscall.Mmap(int(f.Fd()), 0, totalShmSize, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		_ = f.Close()
		return nil, fmt.Errorf("mmap failed: %w", err)
	}

	binary.LittleEndian.PutUint32(data[0:4], magicBAWL)
	binary.LittleEndian.PutUint32(data[4:8], 1)
	binary.LittleEndian.PutUint32(data[8:12], numSlots)
	binary.LittleEndian.PutUint32(data[12:16], slotCapacity)

	return &ShmProducer{
		file:     f,
		mmapData: data,
	}, nil
}

// ConnectNotifyListener keeps a lazy Unix-socket connection to the vision
// consumer. It reconnects automatically when the vision service restarts.
func (p *ShmProducer) ConnectNotifyListener() {
	go func() {
		for {
			p.mu.Lock()
			connected := p.notifyConn != nil
			p.mu.Unlock()
			if connected {
				time.Sleep(500 * time.Millisecond)
				continue
			}

			conn, err := net.DialTimeout("unix", sockPath, 200*time.Millisecond)
			if err != nil {
				time.Sleep(500 * time.Millisecond)
				continue
			}

			p.mu.Lock()
			if p.notifyConn == nil {
				p.notifyConn = conn
			} else {
				_ = conn.Close()
			}
			p.mu.Unlock()
		}
	}()
}

func (p *ShmProducer) WriteFrame(frameID uint32, frameBytes []byte) error {
	if len(frameBytes) > slotCapacity {
		return fmt.Errorf("frame size %d exceeds slot capacity %d", len(frameBytes), slotCapacity)
	}

	p.mu.Lock()
	defer p.mu.Unlock()

	seq := p.writeSeq
	slotIdx := seq & (numSlots - 1)
	slotOffset := globalHdrSz + (slotIdx * slotStride)

	slotHdr := p.mmapData[slotOffset : slotOffset+slotHeaderSz]
	slotBuf := p.mmapData[slotOffset+slotHeaderSz : slotOffset+slotStride]

	// Mark the slot as being written before touching the payload.
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&slotHdr[0])), 0)

	copy(slotBuf[:len(frameBytes)], frameBytes)

	binary.LittleEndian.PutUint32(slotHdr[4:8], frameID)
	binary.LittleEndian.PutUint64(slotHdr[8:16], uint64(time.Now().UnixNano()))
	binary.LittleEndian.PutUint32(slotHdr[16:20], uint32(len(frameBytes)))
	slotHdr[24] = 0
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&slotHdr[0])), 1)

	p.writeSeq++
	atomic.StoreUint64((*uint64)(unsafe.Pointer(&p.mmapData[16])), p.writeSeq)

	if p.notifyConn != nil {
		var token [8]byte
		binary.LittleEndian.PutUint64(token[:], p.writeSeq)
		_ = p.notifyConn.SetWriteDeadline(time.Now().Add(5 * time.Millisecond))
		if _, err := p.notifyConn.Write(token[:]); err != nil {
			// Consumer restarted or its socket buffer is full: drop this
			// notification and let the reconnect loop establish a fresh link.
			_ = p.notifyConn.Close()
			p.notifyConn = nil
		}
	}

	return nil
}

func (p *ShmProducer) Close() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.notifyConn != nil {
		_ = p.notifyConn.Close()
		p.notifyConn = nil
	}
	_ = syscall.Munmap(p.mmapData)
	_ = p.file.Close()
}

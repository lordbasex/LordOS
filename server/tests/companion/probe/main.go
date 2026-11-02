// A test companion for tests/companion_e2e.sh: a static Go program that tries what the sandbox must allow and what it
// must refuse, from inside, and writes one line per check to ~/Downloads/companion-report.txt. It then answers on fd 3 and,
// with Pion, connects two peers in this process over the machine's own addresses and passes data, which is what an engine does.
package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"syscall"
	"time"
	"unsafe"

	"github.com/pion/webrtc/v4"
)

var report bytes.Buffer

func check(name string, ok bool, detail any) {
	mark := "ok  "
	if !ok {
		mark = "FAIL"
	}
	fmt.Fprintf(&report, "%s %s (%v)\n", mark, name, detail)
}

func denied(name string, err error) { check(name, err != nil, err) }
func works(name string, err error)  { check(name, err == nil, err) }

// Nothing inherited past 4; the Go runtime's own epoll and eventfd have no file type.
func inherited() {
	open := 0
	for fd := 5; fd < 256; fd++ {
		var st syscall.Stat_t
		if syscall.Fstat(fd, &st) == nil && st.Mode&syscall.S_IFMT != 0 {
			open++
		}
	}
	check("no inherited descriptor past 4", open == 0, open)
}

func files(home string) {
	dl := filepath.Join(home, "Downloads")
	_, err := os.ReadFile(filepath.Join(home, "probe-home.txt"))
	works("read a file in the home folder", err)
	_, err = os.ReadDir(home)
	works("list the home folder", err)
	for _, p := range []string{"/etc/passwd", "/etc/shadow", "/proc/self/status", "/proc/1/environ", "/sys/kernel/mm", "/tmp", "/usr/bin", "/usr/lib", "/dev/null", "/dev/urandom", "/var/lib/lordos", "/run/lordos"} {
		_, err = os.ReadFile(p)
		if e, ok := err.(*os.PathError); ok && e.Err == syscall.EISDIR {
			_, err = os.ReadDir(p)
		}
		denied("read "+p, err)
	}
	if _, err := os.ReadFile("/etc/ssl/certs/ca-certificates.crt"); err != nil && !os.IsNotExist(err) {
		check("read TLS roots", false, err)
	} else {
		check("read TLS roots", true, err)
	}
	works("write in Downloads", os.WriteFile(filepath.Join(dl, "w.txt"), []byte("x"), 0o644))
	works("make a folder in Downloads", os.Mkdir(filepath.Join(dl, "d"), 0o755))
	works("rename in Downloads", os.Rename(filepath.Join(dl, "w.txt"), filepath.Join(dl, "d", "r.txt")))
	works("truncate in Downloads", os.Truncate(filepath.Join(dl, "d", "r.txt"), 0))
	works("delete in Downloads", os.RemoveAll(filepath.Join(dl, "d")))
	denied("write in the home folder", os.WriteFile(filepath.Join(home, "x.txt"), []byte("x"), 0o644))
	denied("change a file in the home folder", os.WriteFile(filepath.Join(home, "probe-home.txt"), []byte("x"), 0o644))
	denied("truncate a file in the home folder", os.Truncate(filepath.Join(home, "probe-home.txt"), 0))
	denied("delete a file in the home folder", os.Remove(filepath.Join(home, "probe-home.txt")))
	denied("write appdata", os.WriteFile(filepath.Join(home, ".local/share/lordos/appdata/pkgtest/x"), []byte("x"), 0o644))
	denied("write /tmp", os.WriteFile("/tmp/companion-x", []byte("x"), 0o644))
	denied("symlink in Downloads", os.Symlink("/etc/passwd", filepath.Join(dl, "l")))
	denied("hard link from home into Downloads", os.Link(filepath.Join(home, "probe-home.txt"), filepath.Join(dl, "hl")))
	denied("rename from home into Downloads", os.Rename(filepath.Join(home, "probe-home.txt"), filepath.Join(dl, "m")))
	denied("chmod in the home folder", os.Chmod(filepath.Join(home, "probe-home.txt"), 0o777))
	denied("chown in the home folder", os.Chown(filepath.Join(home, "probe-home.txt"), os.Getuid(), os.Getgid()))
	denied("change times in the home folder", os.Chtimes(filepath.Join(home, "probe-home.txt"), time.Unix(1, 0), time.Unix(1, 0)))
	denied("a fifo in Downloads", syscall.Mkfifo(filepath.Join(dl, "f"), 0o644))
}

func execs(home string) {
	argv := []string{"sh", "-c", "true"}
	denied("execve /bin/sh", syscall.Exec("/bin/sh", argv, nil))
	self := filepath.Join(home, "Downloads", "copy")
	if os.WriteFile(self, []byte("#!/bin/sh\n"), 0o755) == nil {
		denied("execve a file written in Downloads", syscall.Exec(self, []string{"copy"}, nil))
		os.Remove(self)
	}
	_, err := syscall.ForkExec("/bin/true", []string{"true"}, &syscall.ProcAttr{})
	denied("fork and exec", err)
	_, _, e := syscall.RawSyscall(syscall.SYS_FORK, 0, 0, 0)
	check("fork", e != 0, e)
	name := []byte("x\x00")
	_, _, e = syscall.Syscall(319 /* memfd_create, amd64 */, uintptr(unsafe.Pointer(&name[0])), 0, 0)
	check("memfd_create", e != 0, e)
	empty := []byte{0}
	_, _, e = syscall.Syscall6(322 /* execveat, amd64 */, 3, uintptr(unsafe.Pointer(&empty[0])), 0, 0, 0x1000, 0)
	check("execveat of another descriptor", e != 0, e)
}

func sockets() {
	for _, d := range []struct {
		n string
		f int
	}{{"AF_UNIX", syscall.AF_UNIX}, {"AF_PACKET", syscall.AF_PACKET}, {"AF_VSOCK", 40}, {"AF_ALG", 38}, {"AF_KEY", 15}} {
		fd, err := syscall.Socket(d.f, syscall.SOCK_STREAM, 0)
		if err == nil {
			syscall.Close(fd)
		}
		denied("socket "+d.n, err)
	}
	_, err := syscall.Socket(syscall.AF_NETLINK, syscall.SOCK_RAW, 9 /* audit */)
	denied("netlink audit", err)
	_, err = syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	denied("socketpair", err)
	_, err = net.Dial("unix", os.Getenv("HOME")+"/rt/lordos/remote.sock")
	denied("connect a named socket", err)
	_, err = net.Dial("unix", "/run/dbus/system_bus_socket")
	denied("connect the system bus", err)
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if l != nil {
		l.Close()
	}
	denied("listen", err)
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_DGRAM, 0)
	works("socket AF_INET", err)
	if err == nil {
		syscall.Close(fd)
	}
	ifs, err := net.Interfaces()
	check("list interfaces through netlink", err == nil && len(ifs) > 0, len(ifs))
}

func processes() {
	_, _, e := syscall.Syscall(syscall.SYS_KILL, 1, 0, 0)
	check("kill", e != 0, e)
	_, _, e = syscall.Syscall(syscall.SYS_TGKILL, 1, 1, 0)
	check("tgkill another process", e != 0, e)
	_, _, e = syscall.Syscall6(syscall.SYS_PRLIMIT64, 1, 0, 0, 0, 0, 0)
	check("prlimit of another process", e != 0, e)
	_, _, e = syscall.Syscall6(syscall.SYS_PTRACE, 16 /* attach */, uintptr(os.Getppid()), 0, 0, 0, 0)
	check("ptrace", e != 0, e)
	_, _, e = syscall.Syscall(syscall.SYS_UNSHARE, syscall.CLONE_NEWUSER, 0, 0)
	check("unshare", e != 0, e)
	_, _, e = syscall.Syscall(syscall.SYS_IOCTL, 3, 0x541B /* FIONREAD */, 0)
	check("ioctl", e == syscall.EPERM, e)
	check("no capabilities or uid 0", os.Geteuid() != 0, os.Geteuid())
	for _, e := range []string{"XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "WAYLAND_DISPLAY"} {
		check("no "+e, os.Getenv(e) == "", os.Getenv(e))
	}
}

func readMsg(c net.Conn) (int, []byte, error) {
	h := make([]byte, 5)
	if _, err := io.ReadFull(c, h); err != nil {
		return 0, nil, err
	}
	b := make([]byte, binary.LittleEndian.Uint32(h)-1)
	_, err := io.ReadFull(c, b)
	return int(h[4]), b, err
}

// fd 4: the sharing API, already tied to this app. SHARE_QUERY → SHARE_STATE; then a start, whose result the test reads:
// frames with a grant, a refusal without one. Writes "share: <result>" to the report.
func share() {
	f := os.NewFile(4, "share")
	c, err := net.FileConn(f)
	if err != nil {
		fmt.Fprintf(&report, "share: no fd 4\n")
		return
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(8 * time.Second))
	c.Write([]byte{1, 0, 0, 0, 51})
	t, b, err := readMsg(c)
	check("fd 4 answers SHARE_QUERY", err == nil && t == 60 && len(b) == 10, t)
	start := []byte{9, 0, 0, 0, 50, 1 | 4, 1, 30, 0, 0xd0, 7, 0, 0}
	c.Write(start)
	for {
		t, b, err = readMsg(c)
		if err != nil {
			fmt.Fprintf(&report, "share: %v\n", err)
			return
		}
		if t == 40 /* RP_VFRAME */ {
			fmt.Fprintf(&report, "share: frames\n")
			c.Write([]byte{1, 0, 0, 0, 52})
			return
		}
		if t == 61 {
			fmt.Fprintf(&report, "share: refused %d\n", b[0])
			return
		}
	}
}

func pion() {
	cfg := webrtc.Configuration{}
	a, err := webrtc.NewPeerConnection(cfg)
	if err != nil {
		check("pion: peer", false, err)
		return
	}
	b, _ := webrtc.NewPeerConnection(cfg)
	defer a.Close()
	defer b.Close()
	got := make(chan string, 1)
	b.OnDataChannel(func(d *webrtc.DataChannel) {
		d.OnMessage(func(m webrtc.DataChannelMessage) { got <- string(m.Data) })
	})
	dc, _ := a.CreateDataChannel("t", nil)
	dc.OnOpen(func() { dc.SendText("through DTLS and SCTP") })
	offer, _ := a.CreateOffer(nil)
	wa := webrtc.GatheringCompletePromise(a)
	a.SetLocalDescription(offer)
	<-wa
	b.SetRemoteDescription(*a.LocalDescription())
	ans, _ := b.CreateAnswer(nil)
	wb := webrtc.GatheringCompletePromise(b)
	b.SetLocalDescription(ans)
	<-wb
	a.SetRemoteDescription(*b.LocalDescription())
	select {
	case m := <-got:
		check("pion: data channel between two peers", m == "through DTLS and SCTP", m)
	case <-time.After(15 * time.Second):
		check("pion: data channel between two peers", false, "timeout")
	}
}

func main() {
	inherited()
	home := os.Getenv("HOME")
	app := os.NewFile(3, "app")
	c, err := net.FileConn(app)
	check("fd 3", err == nil, err)
	files(home)
	execs(home)
	sockets()
	processes()
	share()
	pion()
	fmt.Fprintln(&report, "end")
	os.WriteFile(filepath.Join(home, "Downloads", "companion-report.txt"), report.Bytes(), 0o644)
	// the app's end: answer, then wait; the end of the socket means the end
	buf := make([]byte, 65536)
	for {
		n, err := c.Read(buf)
		if err != nil || n == 0 {
			os.WriteFile(filepath.Join(home, "Downloads", "companion-eof.txt"), []byte("eof"), 0o644)
			return
		}
		c.Write([]byte(`{"pong":1}`))
	}
}

package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

const setPageCount = 8
const setPageSlots = 32

// The device's switch thread and this editor both lock the same file so a save
// can never overlap the window in which Move has changed page in memory but has
// not yet written current_page.txt.
const setPagesLockName = ".set-pages.lock"

var setPagesLockWait = 2 * time.Second

func acquireSetPagesLock(root string, exclusive bool) (*os.File, error) {
	f, err := os.OpenFile(filepath.Join(root, setPagesLockName), os.O_CREATE|os.O_RDWR, 0666)
	if err != nil {
		return nil, err
	}
	how := syscall.LOCK_SH
	if exclusive {
		how = syscall.LOCK_EX
	}
	deadline := time.Now().Add(setPagesLockWait)
	for {
		err = syscall.Flock(int(f.Fd()), how|syscall.LOCK_NB)
		if err == nil {
			return f, nil
		}
		if err == syscall.EINTR {
			continue
		}
		if err != syscall.EWOULDBLOCK && err != syscall.EAGAIN {
			_ = f.Close()
			return nil, err
		}
		if !time.Now().Before(deadline) {
			_ = f.Close()
			return nil, errors.New("a Set page change is in progress; reload after Move finishes")
		}
		time.Sleep(25 * time.Millisecond)
	}
}

func releaseSetPagesLock(f *os.File) {
	if f == nil {
		return
	}
	_ = syscall.Flock(int(f.Fd()), syscall.LOCK_UN)
	_ = f.Close()
}

type setPageItem struct {
	ID    string `json:"id"`
	Name  string `json:"name"`
	Color *int   `json:"color,omitempty"`
}
type setPageSnapshot struct {
	Current  int              `json:"current"`
	Revision string           `json:"revision"`
	Pages    [][]*setPageItem `json:"pages"`
	attrs    map[string]map[string]string
	location map[string]int
}

var setPagesMu sync.Mutex
var setPageReadXattr = func(path, key string) (string, error) {
	b, err := exec.Command("getfattr", "--only-values", "-n", key, path).Output()
	return strings.TrimSpace(string(b)), err
}

func readActiveSetPage(root string, slots []*setPageItem) error {
	dir := filepath.Join(filepath.Dir(root), "UserLibrary", "Sets")
	entries, err := os.ReadDir(dir)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if !entry.IsDir() || strings.HasPrefix(entry.Name(), ".") {
			continue
		}
		path := filepath.Join(dir, entry.Name())
		raw, err := setPageReadXattr(path, "user.song-index")
		if err != nil {
			continue
		}
		i, err := strconv.Atoi(raw)
		if err != nil || i < 0 || i >= len(slots) {
			continue
		}
		if slots[i] != nil {
			return fmt.Errorf("duplicate slot on active page")
		}
		subs, err := os.ReadDir(path)
		if err != nil {
			return err
		}
		name := entry.Name()
		for _, sub := range subs {
			if sub.IsDir() && !strings.HasPrefix(sub.Name(), ".") {
				name = sub.Name()
				break
			}
		}
		item := &setPageItem{ID: entry.Name(), Name: name}
		if value, e := setPageReadXattr(path, "user.song-color"); e == nil {
			if color, e := strconv.Atoi(value); e == nil && color >= 0 && color <= 127 {
				item.Color = &color
			}
		}
		slots[i] = item
	}
	return nil
}

func setPageDir(root string, page int) string {
	return filepath.Join(root, "set_pages", fmt.Sprintf("page_%d", page))
}
func safeSetID(id string) bool {
	return id != "" && id != "." && id != ".." && !strings.ContainsAny(id, "/\\ \t\r\n")
}

func readSetPages(root string) (*setPageSnapshot, error) {
	lock, err := acquireSetPagesLock(root, false)
	if err != nil {
		return nil, err
	}
	defer releaseSetPagesLock(lock)
	return readSetPagesLocked(root)
}

func readSetPageAttrs(path string) (map[string]map[string]string, error) {
	b, err := os.ReadFile(path)
	if err != nil && !os.IsNotExist(err) {
		return nil, err
	}
	attrs := map[string]map[string]string{}
	for _, line := range strings.Split(string(b), "\n") {
		fields := strings.SplitN(strings.TrimSuffix(line, "\r"), " ", 3)
		if len(fields) != 3 || fields[0] == "" || fields[1] == "" || !safeSetID(fields[0]) {
			continue
		}
		if attrs[fields[0]] == nil {
			attrs[fields[0]] = map[string]string{}
		}
		attrs[fields[0]][fields[1]] = fields[2]
	}
	return attrs, nil
}

func setPageSlot(raw string) (int, bool) {
	slot, err := strconv.Atoi(strings.TrimSpace(raw))
	if err != nil || slot < 0 || slot >= setPageSlots {
		return 0, false
	}
	return slot, true
}

// The digest includes metadata for every file in the inactive stashes, so even
// changes that do not affect the visible layout invalidate a draft. Reading the
// file contents made every load and save too slow on a device with many Sets;
// size and modification time are enough to notice a change.
func readSetPagesLocked(root string) (*setPageSnapshot, error) {
	entries, err := os.ReadDir(filepath.Join(root, "set_pages"))
	if err != nil && !os.IsNotExist(err) {
		return nil, err
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".manager-edit-") {
			return nil, fmt.Errorf("unfinished edit in set_pages/%s; restore its saved files before continuing", entry.Name())
		}
	}
	s := &setPageSnapshot{Pages: make([][]*setPageItem, setPageCount), attrs: map[string]map[string]string{}, location: map[string]int{}}
	b, err := os.ReadFile(filepath.Join(root, "set_pages", "current_page.txt"))
	if err != nil && !os.IsNotExist(err) {
		return nil, err
	}
	if err == nil {
		s.Current, err = strconv.Atoi(strings.TrimSpace(string(b)))
		if err != nil || s.Current < 0 || s.Current >= setPageCount {
			return nil, errors.New("invalid current page")
		}
	}
	h := sha256.New()
	fmt.Fprintf(h, "current:%d\n", s.Current)
	if info, e := os.Stat(filepath.Join(root, "set_pages", "current_page.txt")); e == nil {
		fmt.Fprintf(h, "current-mtime:%d\n", info.ModTime().UnixNano())
	}
	for p := 0; p < setPageCount; p++ {
		s.Pages[p] = make([]*setPageItem, setPageSlots)
		if p == s.Current {
			if e := readActiveSetPage(root, s.Pages[p]); e != nil {
				return nil, e
			}
			continue
		}
		dir := setPageDir(root, p)
		entries, e := os.ReadDir(dir)
		if os.IsNotExist(e) {
			fmt.Fprintf(h, "missing:%d\n", p)
			continue
		}
		if e != nil {
			return nil, e
		}
		e = filepath.WalkDir(dir, func(path string, d os.DirEntry, walkErr error) error {
			if walkErr != nil {
				return walkErr
			}
			rel, e := filepath.Rel(dir, path)
			if e != nil {
				return e
			}
			info, e := d.Info()
			if e != nil {
				return e
			}
			if info.Mode()&os.ModeSymlink != 0 {
				return fmt.Errorf("symlink in page %d", p+1)
			}
			fmt.Fprintf(h, "%d:%s:%d:%d:%d\n", p, rel, info.Mode(), info.Size(), info.ModTime().UnixNano())
			if !d.IsDir() && !info.Mode().IsRegular() {
				return errors.New("unsupported file in set page")
			}
			return nil
		})
		if e != nil {
			return nil, e
		}
		attrs, e := readSetPageAttrs(filepath.Join(dir, "xattrs.txt"))
		if e != nil {
			return nil, e
		}
		used := make([]bool, setPageSlots)
		for _, entry := range entries {
			if !entry.IsDir() || strings.HasPrefix(entry.Name(), ".") {
				continue
			}
			id := entry.Name()
			if !safeSetID(id) {
				return nil, errors.New("invalid set id")
			}
			if _, exists := s.location[id]; exists {
				return nil, fmt.Errorf("duplicate set %s", id)
			}
			path := filepath.Join(dir, id)
			setAttrs := attrs[id]
			if setAttrs == nil {
				setAttrs = map[string]string{}
			}
			// A Set directory without metadata is what a collision in the
			// device's move leaves behind (and its restore ignores the delta).
			// Fall back to the attributes still on the directory, then to the
			// first free slot, so one bad line cannot break every page.
			slot, ok := setPageSlot(setAttrs["user.song-index"])
			if !ok {
				if raw, readErr := setPageReadXattr(path, "user.song-index"); readErr == nil {
					slot, ok = setPageSlot(raw)
				}
			}
			if !ok || used[slot] {
				slot = -1
				for i := range used {
					if !used[i] {
						slot = i
						break
					}
				}
				if slot < 0 {
					return nil, fmt.Errorf("too many sets on page %d", p+1)
				}
			}
			used[slot] = true
			setAttrs["user.song-index"] = strconv.Itoa(slot)
			color, colorOK := 0, false
			if raw, exists := setAttrs["user.song-color"]; exists {
				if color, e = strconv.Atoi(raw); e == nil && color >= 0 && color <= 127 {
					colorOK = true
				}
			}
			if !colorOK {
				if raw, readErr := setPageReadXattr(path, "user.song-color"); readErr == nil {
					if color, e = strconv.Atoi(raw); e == nil && color >= 0 && color <= 127 {
						colorOK = true
						setAttrs["user.song-color"] = raw
					}
				}
			}
			if !colorOK {
				delete(setAttrs, "user.song-color")
			}
			sub, e := os.ReadDir(path)
			if e != nil {
				return nil, e
			}
			name := id
			for _, item := range sub {
				if item.IsDir() && !strings.HasPrefix(item.Name(), ".") {
					name = item.Name()
					break
				}
			}
			item := &setPageItem{ID: id, Name: name}
			if colorOK {
				item.Color = &color
			}
			s.location[id] = p
			s.attrs[id] = setAttrs
			s.Pages[p][slot] = item
		}
		// Metadata lines with no matching directory are left behind when the
		// device's move hits a collision. Its restore ignores them, and so do
		// we: they are dropped when xattrs.txt is regenerated on save.
	}
	s.Revision = hex.EncodeToString(h.Sum(nil))
	return s, nil
}

func saveSetPages(root, revision string, layout [][]string) (result error) {
	setPagesMu.Lock()
	defer setPagesMu.Unlock()
	lock, err := acquireSetPagesLock(root, true)
	if err != nil {
		return err
	}
	defer releaseSetPagesLock(lock)
	s, err := readSetPagesLocked(root)
	if err != nil {
		return err
	}
	if revision == "" || revision != s.Revision {
		return errors.New("pages changed; refresh and try again")
	}
	if len(layout) != setPageCount {
		return errors.New("invalid page count")
	}
	destination := map[string]int{}
	slots := map[string]int{}
	for p, page := range layout {
		if len(page) != setPageSlots {
			return errors.New("invalid slot count")
		}
		for slot, id := range page {
			if id == "" {
				continue
			}
			if p == s.Current {
				return errors.New("cannot edit the active page")
			}
			if _, ok := s.location[id]; !ok {
				return errors.New("unknown set")
			}
			if _, ok := destination[id]; ok {
				return errors.New("duplicate set")
			}
			destination[id] = p
			slots[id] = slot
		}
	}
	if len(destination) != len(s.location) {
		return errors.New("layout must include every inactive set")
	}
	// A private staging directory permits swaps and rollback without overwrites.
	base := filepath.Join(root, "set_pages")
	stage, err := os.MkdirTemp(base, ".manager-edit-")
	if err != nil {
		return err
	}
	type moved struct{ from, to string }
	moves := []moved{}
	type metadataBackup struct {
		target, backup string
		existed        bool
	}
	replaced := []metadataBackup{}
	// Undo metadata replacements first, then reverse every directory move. A
	// clean undo removes the staging directory too, so a rejected save (such as
	// a destination collision) does not block every later load. The recovery
	// files stay only when a rename failed and a human needs them.
	undo := func() bool {
		ok := true
		for i := len(replaced) - 1; i >= 0; i-- {
			b := replaced[i]
			if e := os.Remove(b.target); e != nil && !os.IsNotExist(e) {
				ok = false
			}
			if b.existed {
				if e := os.Rename(b.backup, b.target); e != nil {
					ok = false
				}
			}
		}
		for i := len(moves) - 1; i >= 0; i-- {
			if e := os.Rename(moves[i].to, moves[i].from); e != nil {
				ok = false
			}
		}
		return ok
	}
	success := false
	defer func() {
		if success || undo() {
			_ = os.RemoveAll(stage)
			return
		}
		if result != nil {
			result = fmt.Errorf("%w; recovery files: %s", result, stage)
		}
	}()
	journal, err := json.Marshal(struct {
		Source      map[string]int `json:"source"`
		Destination map[string]int `json:"destination"`
	}{s.location, destination})
	if err != nil {
		return err
	}
	if err = os.WriteFile(filepath.Join(stage, "journal.json"), journal, 0600); err != nil {
		return err
	}
	ids := make([]string, 0, len(destination))
	for id := range destination {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for p := 0; p < setPageCount; p++ {
		if p != s.Current {
			if err = os.MkdirAll(setPageDir(root, p), 0755); err != nil {
				return err
			}
		}
	}
	for _, id := range ids {
		src := filepath.Join(setPageDir(root, s.location[id]), id)
		tmp := filepath.Join(stage, id)
		if err = os.Rename(src, tmp); err != nil {
			return err
		}
		moves = append(moves, moved{src, tmp})
	}
	for _, id := range ids {
		src := filepath.Join(stage, id)
		dst := filepath.Join(setPageDir(root, destination[id]), id)
		if _, e := os.Lstat(dst); e == nil {
			return errors.New("destination collision")
		} else if !os.IsNotExist(e) {
			return e
		}
		if err = os.Rename(src, dst); err != nil {
			return err
		}
		moves = append(moves, moved{src, dst})
	}
	// Prepare metadata and recovery manifests before replacing originals.
	for p := 0; p < setPageCount; p++ {
		if p == s.Current {
			continue
		}
		var b, manifest strings.Builder
		fmt.Fprintf(&manifest, "# Set page manifest - page %d\n", p)
		for _, id := range ids {
			if destination[id] != p {
				continue
			}
			fmt.Fprintln(&manifest, id)
			attrs := s.attrs[id]
			keys := make([]string, 0, len(attrs))
			for key := range attrs {
				keys = append(keys, key)
			}
			sort.Strings(keys)
			for _, key := range keys {
				v := attrs[key]
				if key == "user.song-index" {
					v = strconv.Itoa(slots[id])
				}
				fmt.Fprintf(&b, "%s %s %s\n", id, key, v)
			}
		}
		for name, contents := range map[string]string{"xattrs.txt": b.String(), "manifest.txt": manifest.String()} {
			if err = os.WriteFile(filepath.Join(stage, fmt.Sprintf("new-%d-%s", p, name)), []byte(contents), 0644); err != nil {
				return err
			}
		}
	}
	for p := 0; p < setPageCount; p++ {
		if p == s.Current {
			continue
		}
		for _, name := range []string{"xattrs.txt", "manifest.txt"} {
			target := filepath.Join(setPageDir(root, p), name)
			backup := filepath.Join(stage, fmt.Sprintf("backup-%d-%s", p, name))
			e := os.Rename(target, backup)
			if e != nil && !os.IsNotExist(e) {
				return e
			}
			replaced = append(replaced, metadataBackup{target, backup, e == nil})
			if err = os.Rename(filepath.Join(stage, fmt.Sprintf("new-%d-%s", p, name)), target); err != nil {
				return err
			}
		}
	}

	success = true
	return nil
}

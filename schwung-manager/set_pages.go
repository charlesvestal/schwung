package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
)

const setPageCount = 8
const setPageSlots = 32

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

func setPageDir(root string, page int) string {
	return filepath.Join(root, "set_pages", fmt.Sprintf("page_%d", page))
}
func safeSetID(id string) bool {
	return id != "" && id != "." && id != ".." && !strings.ContainsAny(id, "/\\ \t\r\n")
}

// The digest includes all files in the inactive stashes and their metadata, so
// even changes that do not affect the visible layout invalidate a draft.
func readSetPages(root string) (*setPageSnapshot, error) {
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
			if info.Mode().IsRegular() {
				f, e := os.Open(path)
				if e != nil {
					return e
				}
				_, e = io.Copy(h, f)
				closeErr := f.Close()
				if e != nil {
					return e
				}
				return closeErr
			}
			if !d.IsDir() {
				return errors.New("unsupported file in set page")
			}
			return nil
		})
		if e != nil {
			return nil, e
		}
		attrData, e := os.ReadFile(filepath.Join(dir, "xattrs.txt"))
		if e != nil && !os.IsNotExist(e) {
			return nil, e
		}
		for _, line := range strings.Split(string(attrData), "\n") {
			fields := strings.SplitN(line, " ", 3)
			if len(fields) != 3 {
				if strings.TrimSpace(line) != "" {
					return nil, fmt.Errorf("invalid metadata on page %d", p+1)
				}
				continue
			}
			if !safeSetID(fields[0]) {
				return nil, errors.New("invalid metadata id")
			}
			if oldPage, exists := s.location[fields[0]]; exists && oldPage != p {
				return nil, fmt.Errorf("metadata for %s is on the wrong page", fields[0])
			}
			if s.attrs[fields[0]] == nil {
				s.attrs[fields[0]] = map[string]string{}
			}
			s.attrs[fields[0]][fields[1]] = fields[2]
		}
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
			s.location[id] = p
			attrs := s.attrs[id]
			if attrs == nil {
				return nil, fmt.Errorf("missing slot metadata for %s", id)
			}
			slot, e := strconv.Atoi(attrs["user.song-index"])
			if e != nil || slot < 0 || slot >= setPageSlots {
				return nil, fmt.Errorf("invalid slot for %s", id)
			}
			if s.Pages[p][slot] != nil {
				return nil, fmt.Errorf("duplicate slot on page %d", p+1)
			}
			sub, e := os.ReadDir(filepath.Join(dir, id))
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
			if color, e := strconv.Atoi(attrs["user.song-color"]); e == nil && color >= 0 && color <= 127 {
				item.Color = &color
			}
			s.Pages[p][slot] = item
		}
		for id := range s.attrs {
			if _, exists := s.location[id]; !exists {
				return nil, fmt.Errorf("orphaned metadata for %s", id)
			}
		}
	}
	s.Revision = hex.EncodeToString(h.Sum(nil))
	return s, nil
}

func saveSetPages(root, revision string, layout [][]string) (result error) {
	setPagesMu.Lock()
	defer setPagesMu.Unlock()
	s, err := readSetPages(root)
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
	success := false
	defer func() {
		if success {
			_ = os.RemoveAll(stage)
		} else if result != nil {
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
	type moved struct{ from, to string }
	moves := []moved{}
	rollback := func() {
		for i := len(moves) - 1; i >= 0; i-- {
			_ = os.Rename(moves[i].to, moves[i].from)
		}
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
			rollback()
			return err
		}
		moves = append(moves, moved{src, tmp})
	}
	for _, id := range ids {
		src := filepath.Join(stage, id)
		dst := filepath.Join(setPageDir(root, destination[id]), id)
		if _, e := os.Lstat(dst); e == nil {
			rollback()
			return errors.New("destination collision")
		} else if !os.IsNotExist(e) {
			rollback()
			return e
		}
		if err = os.Rename(src, dst); err != nil {
			rollback()
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
				rollback()
				return err
			}
		}
	}
	type metadataBackup struct {
		target, backup string
		existed        bool
	}
	replaced := []metadataBackup{}
	restore := func() {
		for i := len(replaced) - 1; i >= 0; i-- {
			b := replaced[i]
			_ = os.Remove(b.target)
			if b.existed {
				_ = os.Rename(b.backup, b.target)
			}
		}
		rollback()
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
				restore()
				return e
			}
			replaced = append(replaced, metadataBackup{target, backup, e == nil})
			if err = os.Rename(filepath.Join(stage, fmt.Sprintf("new-%d-%s", p, name)), target); err != nil {
				restore()
				return err
			}
		}
	}

	success = true
	return nil
}

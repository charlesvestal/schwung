# Release Notes Links in Manager GUI

Link the *available* version text on module and host update prompts to the
matching GitHub release, so users can read release notes before deciding to
update.

## Motivation

The Manager GUI (`schwung-manager`, port 7700) shows available upstream
versions on the modules list, module detail page, and system page. Today
there is no way to see what changed in an upstream version from the GUI —
users have to browse to GitHub manually to decide whether an update is
worth the service restart. A single click from the version text is enough,
and since the GUI is already a browser, a plain link is the cheapest path:
no rate-limit cost, no markdown rendering, and GitHub's own release page is
already the best surface for notes.

## Scope

- Every version number in the GUI (installed and available, for modules
  and the host) becomes a link to
  `https://github.com/{repo}/releases/tag/v{version}`.
- Next to each update/upgrade button, add a small `release notes` link
  (the button stays a button; linkifying the button label is awkward).
- No backend changes for modules — `GithubRepo` is already on the
  catalog entry. System page threads the host's `GithubRepo` through from
  the catalog fetch into the template data.
- Dev-built versions with no matching git tag will 404 — accepted as a
  minor annoyance in exchange for consistent "every version is clickable"
  behavior.

## Implementation

### Template helper

Add to the FuncMap in `schwung-manager/main.go`:

```go
"releaseURL": func(repo, version string) string {
    if repo == "" || version == "" {
        return ""
    }
    if !strings.HasPrefix(version, "v") {
        version = "v" + version
    }
    return "https://github.com/" + repo + "/releases/tag/" + version
},
```

The helper normalizes the `v` prefix because `release.json` stores versions
as `0.3.12` while git tags use `v0.3.12` (per the project's release
convention in the parent `CLAUDE.md`).

### `templates/modules.html`

Wrap the two available-version badges in anchors:

- Card view, available modules (around line 80):
  `{{versionStr (releaseMeta .ID $.ReleaseMeta).Version}}` → wrapped in
  `<a href="{{releaseURL .GithubRepo ...}}" target="_blank" rel="noopener noreferrer">`.
- Table view, available modules (around line 147): same treatment.

Installed-version badges on lines 54 and 115 stay plain text.

### `templates/module_detail.html`

- Line 160 `<span class="update-available">X available</span>` → wrap in
  `<a ... class="update-available">`.
- Line 166 `<code>X</code>` (available version when nothing is installed
  yet) → wrap in an anchor.
- After the update button (around line 182), emit a sibling link:
  `<a class="release-notes-link" href="{{releaseURL ...}}" target="_blank" rel="noopener noreferrer">release notes</a>`.
- Same treatment for the install button flow if one exists on this page —
  verify during implementation and add if present.

### `templates/system.html`

- Installed `Version` (line 15) stays plain.
- Next to the "Upgrade to X" button (around line 26), emit the same
  `release notes` sibling link using the host catalog entry's `GithubRepo`
  (`charlesvestal/schwung`, already populated at main.go:744) and
  `.LatestVersion`.

### Styling

Add to `schwung-manager/static/style.css`:

- `.release-notes-link` — small, secondary color, `margin-left` sized to
  separate it from the adjacent button.
- Inline anchors within `.module-version-inline` and `.update-available`
  inherit normal link color but can get a subtle hover underline if they
  aren't already readable as links.

### Link behavior

All links open in a new tab with `target="_blank" rel="noopener noreferrer"`,
matching the existing GitHub repo link at `module_detail.html:32`.

## Out of scope

- Fetching notes inline via the GitHub Releases API (rejected: extra
  network round-trip, 60/hr unauthenticated rate limit, markdown rendering
  cost, and GitHub's own release page is already adequate).
- Changes to `release.json` or catalog format (no new data needed).

## Verification

- Manager builds and serves without template errors.
- Modules list page: available-version badges are clickable and open the
  correct tag page in a new tab.
- Module detail page: "X available" text and the `release notes` link
  both resolve to the same correct tag URL.
- System page: `release notes` link next to the upgrade button resolves
  to `https://github.com/charlesvestal/schwung/releases/tag/v<version>`.
- Confirm manually on `move.local:7700` against a module whose upstream
  tag exists on GitHub, and verify the tab opens to the expected release.

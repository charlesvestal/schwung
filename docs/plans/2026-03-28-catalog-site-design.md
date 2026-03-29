# Schwung Module Catalog Site

## Problem

64+ modules and growing. No signal about what's good or popular. Overwhelming for users. Community feedback suggests a maxforlive.com-style browse experience with discovery and popularity signals.

## Solution

Static site on GitHub Pages. New repo `schwung-catalog-site`. No framework, no build step — just HTML/CSS/vanilla JS.

## Architecture

- `module-catalog.json` fetched from schwung main repo by GitHub Action (daily + on push)
- Download counts fetched from GitHub releases API by same Action, written to `data/download-counts.json`
- Audio previews stored in `audio/` as MP3 files, named by module ID
- Single page reads static JSON files, renders cards

## Page Structure

- Header with Schwung branding
- Category filter tabs (All, Sound Generators, Audio FX, MIDI FX, Overtake, Utilities, Tools)
- Sort options: Popular (downloads), Newest, Alphabetical
- Responsive grid of module cards (3 col desktop, 2 tablet, 1 mobile)

## Module Card

- Name (linked to GitHub repo)
- Category badge (color-coded pill)
- Author
- Description (1-2 lines)
- Download count (icon + number)
- "Requires" note (if applicable)
- Audio preview player (play button + progress bar, one-at-a-time playback)

## Visual Style

Dark base, warm and approachable. Not sterile/techy, not overly casual. Dark background, slightly lighter card surfaces, warm accent colors for interactive elements.

## GitHub Action

Single workflow (`update-data.yml`), triggers daily + on push to main:

1. Fetch `module-catalog.json` from `raw.githubusercontent.com/charlesvestal/schwung/main/module-catalog.json`
2. Loop through modules, fetch download counts from GitHub releases API (uses default GITHUB_TOKEN, 1000 req/hr)
3. Write `data/module-catalog.json` and `data/download-counts.json`
4. Commit & push if changed — triggers Pages deploy

## Repo Structure

```
schwung-catalog-site/
  index.html
  style.css
  app.js
  data/
    module-catalog.json   (fetched by Action)
    download-counts.json  (fetched by Action)
  audio/
    dexed.mp3
    braids.mp3
    ...
  .github/
    workflows/
      update-data.yml
```

## Future Possibilities (not in scope now)

- Anonymous upvote system (Cloudflare Workers + KV)
- Module screenshots/icons
- "New" badges based on catalog changes
- Discord bot integration
- Search

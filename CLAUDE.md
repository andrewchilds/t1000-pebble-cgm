# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

T1000 is a Pebble watchface that displays real-time Dexcom CGM (Continuous Glucose Monitor) data. It connects to Dexcom Share servers to fetch glucose readings and displays them with trend arrows, delta values, and a 2-hour history chart.

## Build Commands

```sh
# Install dependencies
npm install

# Build and sideload to watch (requires Pebble SDK)
npm run sideload
# Equivalent to: pebble clean && pebble build && pebble install --cloudpebble --logs
```

The Pebble SDK can be installed via `pebble-tool` (Python package). The devcontainer configuration shows the setup process using `uv tool install pebble-tool && pebble sdk install latest`.

## Architecture

### Two-Layer Design

The app has two distinct runtime environments that communicate via AppMessage:

1. **C Layer** (`src/c/main.c`) - Runs on the Pebble watch
   - Renders the watchface UI (time, date, glucose value, trend arrow, chart)
   - Receives formatted data from JS layer via AppMessage
   - Handles display updates, battery indicator, and alert vibrations
   - Targets Aplite (Pebble/Pebble 2) and Basalt (Pebble Time) platforms

2. **PebbleKit JS Layer** (`src/pkjs/index.js`) - Runs on the companion phone
   - Authenticates with Dexcom Share API
   - Fetches glucose readings every ~5 minutes (smart polling based on last reading time)
   - Processes data (calculates delta, formats values for mg/dL or mmol/L)
   - Handles vibration alert logic (high alerts, low-soon predictions)
   - Manages configuration via Clay (`src/pkjs/config.js`)
   - Optional Saltie API integration for meal tracking

### Message Keys

Communication between layers uses numbered keys defined in `package.json` under `pebble.messageKeys`. These must stay in sync with the `#define KEY_*` constants in `main.c`.

### Configuration

Settings are managed via pebble-clay library. The configuration schema is in `src/pkjs/config.js`. Settings include:
- Dexcom credentials and server region (US/International)
- Display preferences (units, reversed colors, thresholds)
- Alert settings (high/low-soon vibrations with delays and repeat intervals)

### Build System

Uses Pebble's waf-based build (`wscript`). The build compiles C code for each target platform and bundles JS files with `src/pkjs/index.js` as the entry point.

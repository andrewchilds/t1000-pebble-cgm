# T1000

A Pebble watchface that displays real-time Dexcom CGM glucose data and provides highly configurable High and Low Soon alerts. This is a port of the software written for the [custom T1D smartwatch project](https://andrewchilds.com/posts/building-a-t1d-smartwatch-from-scratch).

![Watch photo](resources/images/watch.jpg)

## Features

![Screenshot](resources/images/screenshot.png) ![Screenshot Reversed](resources/images/screenshot-reversed.png)

- Current glucose value with trend arrow
- Delta (rate of change)
- Time since last reading
- 2 hour CGM history
- Color-coded chart on Pebble Time (green/orange/red for in-range/high/low)
- Supports mg/dL and mmol/L
- Configurable high/low threshold lines
- Configurable high/low alerts

## Missing Features

- Doesn't handle most error states

## Requirements

- Pebble / Pebble 2 (Aplite) or Pebble Time / Time Steel (Basalt)
- Dexcom CGM with Share enabled
- Dexcom Share account credentials

## Installation Instructions

Install from the Rebble app store:
https://apps.rebble.io/en_US/application/6972fd68ae32660009f7c242

## Building

```sh
npm install
pebble clean && pebble build && pebble install --cloudpebble --logs
```

## License

MIT

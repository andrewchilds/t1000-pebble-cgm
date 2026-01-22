# T1000

A Pebble watchface that displays real-time Dexcom CGM glucose data and provides highly configurable High and Low Soon alerts. This is a port of the software written for the [custom T1D smartwatch project](https://andrewchilds.com/posts/building-a-t1d-smartwatch-from-scratch).

![Watch photo](resources/images/watch.jpg)

## Features

![Screenshot](resources/images/screenshot.png) ![Screenshot Reversed](resources/images/screenshot-reversed.png)

- Current glucose value with trend arrow
- Delta (rate of change)
- Time since last reading
- 2 hour CGM history
- Supports mg/dL and mmol/L
- Configurable high/low threshold lines
- Configurable high/low alerts

## Missing Features

- No support for color Pebble displays yet
- Doesn't handle most error states

## Requirements

- B&W Pebble / Core 2 Duo watch (Aplite)
- Dexcom CGM with Share enabled
- Dexcom Share account credentials

## Building

```sh
npm install
pebble clean && pebble build && pebble install --cloudpebble --logs
```

## License

MIT

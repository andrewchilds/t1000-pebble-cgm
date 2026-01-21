# T1000

A Pebble watchface that displays real-time Dexcom CGM glucose data.

![Screenshot](resources/images/screenshot.png) ![Screenshot Reversed](resources/images/screenshot-reversed.png)

## Features

- Current glucose value with trend arrow
- Delta (rate of change)
- Time since last reading
- 90-minute history chart
- Supports mg/dL and mmol/L
- Configurable high/low threshold lines
- Configurable high/low alerts

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

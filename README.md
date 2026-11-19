# Orbit Station

OrbIt firmware for an ESP32-S3 (N16R8) driving five round GC9A01 screens,
controlled entirely through the orbit-api. Spun off from
[info-orbs](https://github.com/brettdottech/info-orbs).

Work in progress; see [PLAN.md](PLAN.md) for the phases and hardware notes.

## Build

PlatformIO, one environment (`s3`):

```
pio run                 # build
pio run -t upload       # build and flash over the native USB socket
pio device monitor      # serial log, 115200 baud
```

## License

AGPL-3.0, the same as info-orbs. See [LICENSE.txt](LICENSE.txt).

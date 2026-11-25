# Clock face files

A clock face file gives the `analogClock` control a dial picture and hand
shapes made somewhere else (OrbIt Command Center's clock face builder). The
board stores it and draws whatever moves; it never acts on a clock face's
name.

Firmware that supports this lists it in `GET /orbit/api/v1/capabilities`:

```json
"clockFaces": {"fileVersion": 1, "builtIn": ["classic", "platform", "minimal"]}
```

Show the clock face picker only when `clockFaces` is there. `builtIn` are the
names the `clockFace` param takes.

## Uploading

```
PUT /orbit/api/v1/screens/{n}/clock-face
Content-Type: application/json
```

The body is the clock face file (below), at most 256 KB. The board checks it
(JSON, hand shapes, and that the dial decodes as a 240x240 PNG), then stores
it byte for byte as `/orbit-clockface{n}.json` in LittleFS, replacing the
screen's previous one. A bad file is refused with `400` and an `error`
message, and the stored file is left as it was.

```json
200 {"screen": 2, "clockFaceHash": "9e98832c511416d0", "bytes": 37534, "shown": true}
```

`clockFaceHash` is the first 16 hex digits (lower case) of the SHA-256 of the
body as sent, so a client can work it out without asking the board. If the
screen shows `analogClock`, its `clockFaceId` and `clockFaceHash` params are
set to the new file and it shows at once (`shown: true`); otherwise the file
waits for a write that picks it.

Other errors: `413` too big, `503` no file system, `507` couldn't write it
(file system full).

## Showing it

The `analogClock` params that pick an uploaded clock face (both settings,
saved with the layout):

| param           | meaning |
|-----------------|---------|
| `clockFaceId`   | the clock face's name, for clients (the board only stores it) |
| `clockFaceHash` | show the uploaded file with this hash; `""` for the built-in `clockFace` |

`GET /orbit/api/v1/screens/{n}` reports `clockFaceHeld`: the hash of the file
the board holds for that screen, or `null`. If it differs from
`clockFaceHash` (a reflashed or wiped board, or a file uploaded to another
screen), the screen shows its built-in `clockFace`; upload the file again.

A screen POST replaces all of a screen's params, so a client switching clock
face sends the colour params too, or classic's colours reset to defaults.

## The file

```json
{
  "version": 1,
  "id": "dress-gold",
  "dial": "<base64 PNG, 240x240>",
  "hands": {
    "hour":   [{"color": "#dfc285", "points": [[0, -0.109], [0.07, 0.053], [0, 0.527]]},
               {"color": "#867042", "points": [[0, -0.109], [-0.07, 0.053], [0, 0.527]]}],
    "minute": [ ... ],
    "second": [{"color": "#f3ead8", "points": [[-0.007, -0.218], [-0.007, 0.909], [0.007, 0.909], [0.007, -0.218]]},
               {"color": "#f3ead8", "disc": [-0.218, 0.027]}]
  },
  "cap": [{"color": "#f3ead8", "disc": [0, 0.05]},
          {"color": "#111111", "disc": [0, 0.018]}]
}
```

- `version`: 1.
- `id` (optional): up to 64 characters.
- `dial`: everything that stays still (background, numerals, ticks), as a
  240x240 PNG in base64. PNG keeps thin lines sharp. Transparency is laid onto
  black.
- `hands.hour`, `hands.minute` (required) and `hands.second` (optional; leave
  it out for no second hand): each 1 to 4 parts, drawn in order.
- `cap` (optional): 1 to 4 parts drawn over the hands at the centre, not
  turned.

A part has a `color` (`"#rrggbb"`) and `points`, a `disc`, or both:

- `points`: 3 to 16 `[x, y]` pairs, an outline filled with smooth edges
  (even-odd, so it may be concave).
- `disc`: `[along, radius]`, a filled circle on the hand's line.

Units: 0 is the centre and 1 is the face's radius, 110 px (the screen is
240 px across, centre at the middle of pixel 120). A hand is described
pointing at 12 o'clock: `y` along it (outwards positive), `x` across it
(right positive). Numbers are limited to -1.5 to 1.5, and disc radii to 0.5.
The board turns the hands: hour and minute move smoothly, the second hand
ticks.

Colours are reduced to the screens' RGB565, so a preview should do the same
(`r & 0xF8, g & 0xFC, b & 0xF8`).

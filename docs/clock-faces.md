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

Per-screen options that work with any clock face, built-in or uploaded (also
settings):

| param      | default  | meaning |
|------------|----------|---------|
| `seconds`  | `true`   | show the second hand, if the clock face has one |
| `motion`   | `"tick"` | `"sweep"`: the second hand moves smoothly (about 10 fps, more drawing on the board) |
| `secondsRing` | `false` | built-in clock faces: a ring round the rim (radius 116, 3 px, the second hand's colour) filling with the seconds; a file has its own `secondsRing` |
| `timeZone` | `""`     | the hands show this timezone (IANA name, e.g. `"Europe/London"`); `""` for the board's own |
| `zone2`    | `""`     | a second timezone: a hand going round once a day (24-hour scale) and the city's name on the dial; only on clock faces with a zone hand (the built-in ones, and files with `hands.zone`) |

An unknown timezone is refused with `400`.

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
- `hands.zone` (optional): the second timezone's hand, pointing at 12 for
  midnight there. Only a clock face with one takes a second timezone: left
  out, the screen's `zone2` shows nothing (no hand, no name). The built-in
  clock faces all take one, an orange arrow.
- `cap` (optional): 1 to 4 parts drawn over the hands at the centre, not
  turned.
- `date` (optional): a window with the day of the month.
- `weekday` (optional): the day's name (`MON`...), over the dial.
- `zoneLabel` (optional): where the second zone's name goes when the screen
  has `zone2` (and the face a `hands.zone`). Left out: orange, 11 px, under
  the centre (120, 170).
- `secondsRing` (optional): a ring round the centre that fills clockwise
  from 12 o'clock with the seconds, ticking or sweeping with the second hand
  and hidden with it (`seconds: false`).

```json
"secondsRing": {"radius": 116, "width": 3, "color": "#e8423a"},
"date": {"x": 175, "y": 120, "width": 30, "height": 22, "size": 15,
         "color": "#111111", "background": "#f5f2ea", "border": "#d8b46a"},
"weekday": {"x": 120, "y": 72, "size": 14, "color": "#6e6a64"},
"zoneLabel": {"x": 120, "y": 170, "size": 11, "color": "#d97757"}
```

The ring's `radius` (10 to 120) and `width` (1 to 12) are in pixels.
`x` and `y` are the middle of the text (or window) in pixels of the 240x240
picture, `size` the font's pixel size (6 to 48), Roboto. A date window's
`width` and `height` (8 to 120, default 30 x 22), `background` and `border`
are optional; without a background the number is drawn straight over the
dial. These are drawn into the board's copy of the dial, so leave room for
them in the picture rather than drawing them into it.

A part has a `color` (`"#rrggbb"`) and `points`, a `disc`, or both:

- `points`: 3 to 16 `[x, y]` pairs, an outline filled with smooth edges
  (it may be concave; where an outline crosses itself, overlapping loops are
  both filled).
- `disc`: `[along, radius]`, a filled circle on the hand's line.

Units: 0 is the centre and 1 is the face's radius, 110 px (the screen is
240 px across, centre at the middle of pixel 120). A hand is described
pointing at 12 o'clock: `y` along it (outwards positive), `x` across it
(right positive). Numbers are limited to -1.5 to 1.5, and disc radii to 0.5.
The board turns the hands: hour and minute move smoothly, the second hand
ticks (or sweeps, with `motion`).

Colours are reduced to the screens' RGB565, so a preview should do the same
(`r & 0xF8, g & 0xFC, b & 0xF8`).

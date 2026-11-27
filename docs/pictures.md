# Pictures

The `picture` control shows your own pictures: photos, graphics or animated
GIFs, one or a slideshow. A client (OrbIt Command Center) crops them to the
round screen and uploads them; the board stores them and shows them. It
never fetches a picture from the internet, so pictures keep working with no
internet connection.

Firmware that supports this lists it in `GET /orbit/api/v1/capabilities`:

```json
"pictures": {"maxBytes": 1048576, "width": 240, "height": 240, "formats": ["jpeg", "png", "gif", "webp"]}
```

Show the picture options only when `pictures` is there.

## The files

Each picture is a finished file, exactly 240x240, at most 1 MB:

| format | notes |
|--------|-------|
| JPEG   | baseline only: progressive, lossless and arithmetic-coded JPEGs are refused. Best for photos (a 240x240 photo is ~20-30 KB) |
| PNG    | best for graphics, text and sharp edges |
| GIF    | animated GIFs play (see "Animation" below) |
| WebP   | lossy or lossless; an animated WebP shows its first frame only |

The board never scales or crops: a client makes the 240x240 file. The screen
is round, so the corners don't show.

## The picture store

Pictures are stored once, in the board's flash, and shared by every screen
that shows them. Each is named by its hash: the first 16 hex digits (lower
case) of the SHA-256 of the file, so a client can work it out itself.

### `PUT /orbit/api/v1/pictures/{hash}`

The body is the picture file itself (not a form, not base64), with its
`Content-Length`. Any `Content-Type` other than `multipart/form-data` works
(`application/octet-stream`, `image/jpeg`...).

The board writes it to flash as it arrives, then checks it: the hash, the
format and the size. A file that fails is removed; nothing half-written is
ever kept.

```json
200 {"hash": "3761362fe84b9dea", "bytes": 5744, "format": "jpeg", "stored": true}
```

`stored` is `false` when the board already had it (nothing is written).
Screens that list the picture and were waiting for it show it at once.

| status | why |
|--------|-----|
| `400` | the hash isn't 16 lowercase hex digits, or doesn't match the file; the body is empty or ended early; it isn't a JPEG, PNG, GIF or WebP, isn't 240x240, or is a progressive JPEG; it was sent as a form |
| `413` | over 1 MB: answered straight away, and the connection closed |
| `500` | the file system isn't available, or writing failed |
| `507` | not enough room, even after removing unused pictures (below) |

**Uploads are slow and hold up the board.** Flash takes ~40-50 KB/s: a photo
is under a second, a 1 MB GIF ~20 s. The board answers nothing else until an
upload ends, and while it writes, animated screens slow down (flash writes
stop both cores for a moment). So:

- upload only what the board doesn't have (check `GET .../pictures`, or a
  screen's `picturesMissing`);
- one at a time;
- before the screen write that lists them, so they show straight away.

### `GET /orbit/api/v1/pictures`

```json
{
  "pictures": [{"hash": "3761362fe84b9dea", "bytes": 5744, "inUse": true}],
  "bytes": 5744,
  "roomBytes": 3315763,
  "maxBytes": 1048576
}
```

`inUse`: a screen lists it. `bytes`: all the pictures together. `roomBytes`:
the most the next upload can be, counting the room unused pictures would give
up.

### `GET /orbit/api/v1/pictures/{hash}`

The file, as stored. `404` if the board doesn't have it.

### `DELETE /orbit/api/v1/pictures/{hash}`

`200 {"removed": "..."}`. `404` if the board doesn't have it, `409` if a
screen lists it (take it off the screen first).

### Room

The store has ~3.3 MB (about 100 photos, or a few big GIFs). Pictures no
screen lists stay as a cache, so switching back to a profile needs no upload.
They are removed, oldest first, only when an upload needs their room. A
picture a screen lists is never removed. 128 KB is always kept free for
drawings and clock face files.

## The `picture` control

| param        | default    | meaning |
|--------------|------------|---------|
| `pictures`   | `[]`       | the pictures to show, by hash, in order (up to 24) |
| `seconds`    | `30`       | how long each stays, 5-86400; an animation loops for that long |
| `order`      | `"list"`   | `"shuffle"`: a new order each round, never the same picture twice in a row |
| `transition` | `"random"` | how one picture changes to the next: `"none"`, or an effect from `transitions` in capabilities |

All are settings, saved with the layout; the control has no live values.
With one picture there's no slideshow: it just shows (and plays, if it's an
animation).

`GET /orbit/api/v1/screens/{n}` also reports `picturesMissing`: the pictures
in `pictures` the board doesn't hold (a new board, a wiped one, or not
uploaded yet). They're skipped until they arrive.

What the screen shows when it has nothing to show:

| message | why |
|---------|-----|
| No pictures | `pictures` is empty |
| Waiting for pictures | the board holds none of them |
| Can't show these pictures | the ones it holds won't decode (logged on the serial port); they're tried again when the list or the store changes |

## Animation

An animated GIF plays, looping, for its `seconds` in a slideshow. One that
comes round again carries on where it paused. Frames shorter than 30 ms
show for 30 ms; 0 or 10 ms frames show for 100 ms, as in browsers.

Each frame is decoded as it's needed (the file stays compressed), which
takes the board ~20-30 ms for a frame that changes the whole screen. So a
full-screen animation plays at about 10-15 fps, below its own rate if that's
faster; one that changes only part of the screen each frame plays closer to
its rate. If the board can't keep up, the animation slows down, never the
other screens. An optimised GIF (only the changed part of each frame stored)
is smaller and faster.

An animated WebP shows its first frame: decoding a WebP frame takes the
board 300-470 ms, far too slow to play.

## Memory

A picture screen keeps the picture it shows decoded (115 KB of PSRAM), plus
the next one in a slideshow (115 KB) and an animation's next frame (115 KB),
plus an animated GIF's file. With less than 1 MB of PSRAM left after reading
a GIF, it shows its first frame instead of playing.

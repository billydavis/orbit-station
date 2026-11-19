#include "Artwork.h"
#include <AnimatedGIF.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include "src/webp/decode.h"
#include "src/webp/demux.h"

namespace makapix {

    namespace {
        constexpr int MAX_SIDE = 256;          // Makapix art is 8x8 to 256x256
        constexpr uint16_t DEFAULT_DELAY = 100; // for frames that don't say

        uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

        // Lays straight (non-premultiplied) RGBA onto black.
        uint16_t overBlack(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
            if (a == 255) return rgb565(r, g, b);
            return rgb565(r * a / 255, g * a / 255, b * a / 255);
        }

        uint16_t *newFrame(int w, int h) {
            return (uint16_t *) heap_caps_calloc((size_t) w * h, 2, MALLOC_CAP_SPIRAM);
        }

        // Adds a frame (copied from canvas) if it fits the budget.
        bool addFrame(Artwork &art, const uint16_t *canvas, uint16_t delay, size_t maxBytes, String &error) {
            const size_t frameBytes = (size_t) art.width * art.height * 2;
            if ((art.frames.size() + 1) * frameBytes > maxBytes) {
                error = "too large to keep in memory";
                return false;
            }
            uint16_t *f = newFrame(art.width, art.height);
            if (!f) {
                error = "out of memory";
                return false;
            }
            memcpy(f, canvas, frameBytes);
            art.frames.push_back(f);
            if (delay < 20) delay = DEFAULT_DELAY; // 0 and tiny delays mean "as fast as possible"; play at 10 fps
            art.delaysMs.push_back(delay);
            art.totalMs += delay;
            return true;
        }

        bool sizeOk(int w, int h, String &error) {
            if (w <= 0 || h <= 0 || w > MAX_SIDE || h > MAX_SIDE) {
                error = "unexpected size " + String(w) + "x" + String(h);
                return false;
            }
            return true;
        }

        // ---------- PNG ----------
        struct PngJob {
            PNG *png;
            uint16_t *canvas;
            int width;
        };

        int pngLine(PNGDRAW *d) {
            PngJob *job = (PngJob *) d->pUser;
            job->png->getLineAsRGB565(d, job->canvas + d->y * job->width, PNG_RGB565_LITTLE_ENDIAN, 0x000000);
            return 1;
        }

        std::shared_ptr<Artwork> decodePng(const uint8_t *data, size_t len, size_t maxBytes, String &error) {
            // PNGdec's state is about 40 KB; keep it off the stack and out of
            // internal RAM.
            PNG *png = (PNG *) heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM);
            if (!png) {
                error = "out of memory";
                return nullptr;
            }
            new (png) PNG();
            std::shared_ptr<Artwork> art;
            if (png->openRAM((uint8_t *) data, len, pngLine) != PNG_SUCCESS) {
                error = "not a readable PNG";
            } else if (sizeOk(png->getWidth(), png->getHeight(), error)) {
                auto a = std::make_shared<Artwork>();
                a->width = png->getWidth();
                a->height = png->getHeight();
                uint16_t *canvas = newFrame(a->width, a->height);
                PngJob job = {png, canvas, a->width};
                if (!canvas) {
                    error = "out of memory";
                } else if (png->decode(&job, 0) != PNG_SUCCESS) {
                    error = "PNG decode failed";
                } else if (addFrame(*a, canvas, 0, maxBytes, error)) {
                    art = a;
                }
                free(canvas);
                png->close();
            }
            png->~PNG();
            free(png);
            return art;
        }

        // ---------- GIF ----------
        struct GifJob {
            uint16_t *canvas;
            int width, height;
            // The frame just drawn, so its disposal can be applied before the next.
            int fx, fy, fw, fh;
            uint8_t disposal;
        };

        void gifLine(GIFDRAW *d) {
            GifJob *job = (GifJob *) d->pUser;
            const int y = d->iY + d->y;
            if (d->y == 0) {
                job->fx = d->iX;
                job->fy = d->iY;
                job->fw = d->iWidth;
                job->fh = d->iHeight;
                job->disposal = d->ucDisposalMethod;
            }
            if (y < 0 || y >= job->height) return;
            uint16_t *row = job->canvas + y * job->width;
            for (int x = 0; x < d->iWidth; x++) {
                const int cx = d->iX + x;
                if (cx < 0 || cx >= job->width) continue;
                const uint8_t p = d->pPixels[x];
                if (d->ucHasTransparency && p == d->ucTransparent) continue; // keep what's beneath
                row[cx] = d->pPalette[p];
            }
        }

        std::shared_ptr<Artwork> decodeGif(const uint8_t *data, size_t len, size_t maxBytes, String &error) {
            AnimatedGIF *gif = (AnimatedGIF *) heap_caps_malloc(sizeof(AnimatedGIF), MALLOC_CAP_SPIRAM);
            if (!gif) {
                error = "out of memory";
                return nullptr;
            }
            new (gif) AnimatedGIF();
            gif->begin(GIF_PALETTE_RGB565_LE);
            std::shared_ptr<Artwork> art;
            if (!gif->open((uint8_t *) data, len, gifLine)) {
                error = "not a readable GIF";
            } else if (sizeOk(gif->getCanvasWidth(), gif->getCanvasHeight(), error)) {
                auto a = std::make_shared<Artwork>();
                a->width = gif->getCanvasWidth();
                a->height = gif->getCanvasHeight();
                GifJob job = {newFrame(a->width, a->height), a->width, a->height, 0, 0, 0, 0, 0};
                uint16_t *previous = newFrame(a->width, a->height); // for disposal 3
                if (!job.canvas || !previous) {
                    error = "out of memory";
                } else {
                    bool ok = true;
                    for (;;) {
                        memcpy(previous, job.canvas, (size_t) a->width * a->height * 2);
                        int delay = 0;
                        const int more = gif->playFrame(false, &delay, &job);
                        if (more < 0) {
                            ok = a->frames.size() > 0; // keep what decoded
                            break;
                        }
                        if (!addFrame(*a, job.canvas, delay, maxBytes, error)) {
                            ok = false;
                            break;
                        }
                        // Disposal for the next frame: 2 = back to background, 3 = undo this frame.
                        if (job.disposal == 2) {
                            for (int y = max(job.fy, 0); y < min(job.fy + job.fh, a->height); y++) {
                                for (int x = max(job.fx, 0); x < min(job.fx + job.fw, a->width); x++) job.canvas[y * a->width + x] = 0;
                            }
                        } else if (job.disposal == 3) {
                            memcpy(job.canvas, previous, (size_t) a->width * a->height * 2);
                        }
                        if (more == 0) break;
                    }
                    if (ok) art = a;
                }
                free(job.canvas);
                free(previous);
                gif->close();
            }
            gif->~AnimatedGIF();
            free(gif);
            return art;
        }

        // ---------- WebP (still or animated) ----------
        std::shared_ptr<Artwork> decodeWebp(const uint8_t *data, size_t len, size_t maxBytes, String &error) {
            WebPData webp = {data, len};
            WebPAnimDecoderOptions opts;
            if (!WebPAnimDecoderOptionsInit(&opts)) {
                error = "WebP decoder unavailable";
                return nullptr;
            }
            opts.color_mode = MODE_RGBA;
            opts.use_threads = 0;
            WebPAnimDecoder *dec = WebPAnimDecoderNew(&webp, &opts);
            if (!dec) {
                error = "not a readable WebP";
                return nullptr;
            }
            WebPAnimInfo info;
            std::shared_ptr<Artwork> art;
            if (!WebPAnimDecoderGetInfo(dec, &info)) {
                error = "not a readable WebP";
            } else if (sizeOk(info.canvas_width, info.canvas_height, error)) {
                auto a = std::make_shared<Artwork>();
                a->width = info.canvas_width;
                a->height = info.canvas_height;
                uint16_t *canvas = newFrame(a->width, a->height);
                bool ok = canvas != nullptr;
                if (!ok) error = "out of memory";
                int previous = 0;
                while (ok && WebPAnimDecoderHasMoreFrames(dec)) {
                    uint8_t *rgba;
                    int timestamp;
                    if (!WebPAnimDecoderGetNext(dec, &rgba, &timestamp)) {
                        error = "WebP decode failed";
                        ok = a->frames.size() > 0;
                        break;
                    }
                    for (int i = 0; i < a->width * a->height; i++, rgba += 4) canvas[i] = overBlack(rgba[0], rgba[1], rgba[2], rgba[3]);
                    ok = addFrame(*a, canvas, timestamp - previous, maxBytes, error);
                    previous = timestamp;
                }
                free(canvas);
                if (ok) art = a;
            }
            WebPAnimDecoderDelete(dec);
            return art;
        }

        // ---------- BMP (uncompressed 24/32-bit, or 8-bit with a palette) ----------
        uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }
        uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

        std::shared_ptr<Artwork> decodeBmp(const uint8_t *data, size_t len, size_t maxBytes, String &error) {
            if (len < 54 || data[0] != 'B' || data[1] != 'M') {
                error = "not a readable BMP";
                return nullptr;
            }
            const uint32_t offset = le32(data + 10), header = le32(data + 14);
            const int32_t w = (int32_t) le32(data + 18), rawH = (int32_t) le32(data + 22);
            const uint16_t bpp = le16(data + 28);
            const uint32_t compression = le32(data + 30);
            const int h = abs(rawH);
            if (!sizeOk(w, h, error)) return nullptr;
            if ((compression != 0 && compression != 3) || (bpp != 24 && bpp != 32 && bpp != 8)) {
                error = "unsupported BMP (" + String(bpp) + "-bit)";
                return nullptr;
            }
            const size_t stride = ((size_t) w * bpp / 8 + 3) & ~3u;
            if (offset + stride * h > len) {
                error = "truncated BMP";
                return nullptr;
            }
            const uint8_t *palette = data + 14 + header;
            auto a = std::make_shared<Artwork>();
            a->width = w;
            a->height = h;
            uint16_t *canvas = newFrame(w, h);
            if (!canvas) {
                error = "out of memory";
                return nullptr;
            }
            for (int y = 0; y < h; y++) {
                const uint8_t *row = data + offset + stride * (rawH > 0 ? h - 1 - y : y); // usually stored bottom-up
                for (int x = 0; x < w; x++) {
                    const uint8_t *p = bpp == 8 ? palette + row[x] * 4 : row + x * (bpp / 8);
                    canvas[y * w + x] = bpp == 32 ? overBlack(p[2], p[1], p[0], p[3]) : rgb565(p[2], p[1], p[0]);
                }
            }
            const bool ok = addFrame(*a, canvas, 0, maxBytes, error);
            free(canvas);
            return ok ? a : nullptr;
        }
    }

    Artwork::~Artwork() {
        for (uint16_t *f : frames) free(f);
    }

    const uint16_t *Artwork::frameAt(uint32_t elapsed) const {
        if (frames.size() <= 1 || totalMs == 0) return frames.empty() ? nullptr : frames[0];
        elapsed %= totalMs;
        for (size_t i = 0; i < frames.size(); i++) {
            if (elapsed < delaysMs[i]) return frames[i];
            elapsed -= delaysMs[i];
        }
        return frames.back();
    }

    uint32_t Artwork::minDelayMs() const {
        if (frames.size() <= 1) return 0;
        uint32_t m = UINT32_MAX;
        for (uint16_t d : delaysMs) m = min(m, (uint32_t) d);
        return m;
    }

    std::shared_ptr<Artwork> decode(const uint8_t *data, size_t len, const String &format, size_t maxBytes, String &error) {
        // Trust the file's own signature over the label.
        if (len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) return decodeWebp(data, len, maxBytes, error);
        if (len >= 8 && !memcmp(data, "\x89PNG", 4)) return decodePng(data, len, maxBytes, error);
        if (len >= 6 && !memcmp(data, "GIF8", 4)) return decodeGif(data, len, maxBytes, error);
        if (len >= 2 && data[0] == 'B' && data[1] == 'M') return decodeBmp(data, len, maxBytes, error);
        error = "unknown image format '" + format + "'";
        return nullptr;
    }

}

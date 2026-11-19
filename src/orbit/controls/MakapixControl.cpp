#include "MakapixControl.h"
#include "makapix/Makapix.h"
#include "ui/Text.h"

namespace orbit {

    namespace {
        constexpr int SIZE = 240;
        const char *const CHANNELS[] = {"all", "promoted", "user", "by_user", "hashtag", "reactions", nullptr};
        const char *const SORTS[] = {"random", "server_order", "created_at", nullptr};

        const char *const FITS[] = {"crisp", "fill", nullptr};

        // Nearest-neighbour: each screen pixel takes the art pixel under it, so
        // pixel art stays sharp. Sprite pixels are stored byte-swapped.
        //   crisp: the largest whole multiple that fits, centred (256 shrinks to fit)
        //   fill:  covers the whole screen, centred; long edges are trimmed
        void blit(LGFX_Sprite &spr, const makapix::Artwork &art, const uint16_t *frame, bool fill) {
            int dw, dh;
            if (fill) {
                // Scale so the shorter side spans the screen.
                if (art.width >= art.height) {
                    dh = SIZE;
                    dw = (art.width * SIZE + art.height - 1) / art.height;
                } else {
                    dw = SIZE;
                    dh = (art.height * SIZE + art.width - 1) / art.width;
                }
            } else {
                const int scale = min(SIZE / art.width, SIZE / art.height);
                if (scale >= 1) {
                    dw = art.width * scale;
                    dh = art.height * scale;
                } else {
                    const int longest = max(art.width, art.height);
                    dw = art.width * SIZE / longest;
                    dh = art.height * SIZE / longest;
                }
            }
            // Where the scaled art sits; negative when it's bigger than the screen.
            const int ox = (SIZE - dw) / 2, oy = (SIZE - dh) / 2;
            const int x0 = max(ox, 0), x1 = min(ox + dw, SIZE), y0 = max(oy, 0), y1 = min(oy + dh, SIZE);
            uint16_t *dst = (uint16_t *) spr.getBuffer();
            uint16_t row[SIZE];
            int lastSy = -1;
            for (int y = y0; y < y1; y++) {
                const int sy = (y - oy) * art.height / dh;
                if (sy != lastSy) {
                    const uint16_t *src = frame + sy * art.width;
                    for (int x = x0; x < x1; x++) row[x] = __builtin_bswap16(src[(x - ox) * art.width / dw]);
                    lastSy = sy;
                }
                memcpy(dst + y * SIZE + x0, row + x0, (x1 - x0) * 2);
            }
        }
    }

    int MakapixControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        int status;
        if ((status = optionalChoice(in, out, "channel", CHANNELS, error))) {
            error = "unknown makapix channel '" + in["channel"].as<String>() + "'";
            return status;
        }
        if ((status = optionalChoice(in, out, "sort", SORTS, error))) {
            error = "unknown makapix sort '" + in["sort"].as<String>() + "'";
            return status;
        }
        if ((status = optionalChoice(in, out, "fit", FITS, error))) {
            error = "unknown makapix fit '" + in["fit"].as<String>() + "' (crisp or fill)";
            return status;
        }
        const String channel = out["channel"].as<String>();
        if (channel == "by_user" || channel == "reactions") {
            if (!in["user"].is<const char *>() || strlen(in["user"]) == 0) {
                error = "makapix channel '" + channel + "' requires params.user (a handle)";
                return 400;
            }
            if ((status = optionalString(in, out, "user", "", 64, error))) return status;
        }
        if (channel == "hashtag") {
            if (!in["hashtag"].is<const char *>() || strlen(in["hashtag"]) == 0) {
                error = "makapix channel 'hashtag' requires params.hashtag";
                return 400;
            }
            String tag = in["hashtag"].as<String>();
            if (tag.startsWith("#")) tag.remove(0, 1);
            out["hashtag"] = tag;
        }
        JsonVariantConst change = in["changeSeconds"];
        if (!change.isNull() && !change.is<long>()) {
            error = "params.changeSeconds must be a whole number";
            return 400;
        }
        out["changeSeconds"] = constrain(change.isNull() ? 120L : change.as<long>(), 10L, 86400L);
        return 0;
    }

    uint32_t MakapixControl::refreshMs(int screen, JsonObjectConst) const { return frameMs[screen]; }

    void MakapixControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &ctx) {
        makapix::Feed feed;
        feed.channel = p["channel"].as<String>();
        feed.user = p["user"] | "";
        feed.hashtag = p["hashtag"] | "";
        feed.sort = p["sort"].as<String>();
        feed.changeMs = p["changeSeconds"].as<uint32_t>() * 1000UL;
        const makapix::Showing s = makapix::show(ctx.screen, feed);

        spr.fillScreen(TFT_BLACK);
        if (s.art) {
            const uint16_t *frame = s.art->frameAt(ctx.nowMs - s.since);
            if (frame) blit(spr, *s.art, frame, strcmp(p["fit"] | "crisp", "fill") == 0);
            // Redraw as often as the art's quickest frame; a still checks for
            // the next artwork twice a second.
            const uint32_t d = s.art->minDelayMs();
            frameMs[ctx.screen] = d ? constrain(d, (uint32_t) 20, (uint32_t) 500) : 500;
            return;
        }
        frameMs[ctx.screen] = 500;
        if (s.code.length()) {
            // Pairing: the code, big enough to read across the room.
            text::draw(spr, "Makapix", 120, 58, 16, Align::MiddleCenter, 0x7BEF, TFT_BLACK);
            text::draw(spr, "Register at", 120, 92, 13, Align::MiddleCenter, TFT_WHITE, TFT_BLACK);
            text::draw(spr, "makapix.club", 120, 114, 13, Align::MiddleCenter, TFT_WHITE, TFT_BLACK);
            text::draw(spr, s.code, 120, 158, 30, Align::MiddleCenter, 0xFEA0, TFT_BLACK);
            return;
        }
        text::draw(spr, s.message, 120, 120, 13, Align::MiddleCenter, 0x7BEF, TFT_BLACK);
    }

}

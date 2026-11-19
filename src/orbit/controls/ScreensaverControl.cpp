#include "ScreensaverControl.h"
#include "ui/Text.h"

namespace orbit {

    namespace {
        constexpr int SIZE = 240;
        constexpr int CENTRE = 120;
        constexpr int PLAYFIELD_RADIUS = 100; // moving things stay inside the bezel
        constexpr float MAX_DT = 0.2f;        // a stall mustn't fling things across the screen
        constexpr uint16_t SILVER = 0xC618, LIGHTGREY = 0xD69A, DARKGREY = 0x7BEF;

        float randAngle() { return random(0, 360) * DEG_TO_RAD; }

        // ================= Asteroids =================
        class Asteroids : public ScreensaverControl::Effect {
            static const int STARS = 40, ROCKS = 4, POINTS = 6, PLANET_R = 24;
            float sx[STARS], sy[STARS];
            uint8_t tier[STARS];
            float driftX, driftY;
            int blink;
            uint32_t blinkUntil, nextBlink;
            float px, py, pvx, pvy;
            float rx[ROCKS], ry[ROCKS], rvx[ROCKS], rvy[ROCKS];
            int8_t ox[ROCKS][POINTS], oy[ROCKS][POINTS];
            int bound[ROCKS];

            static void bounce(float &x, float &y, float &vx, float &vy, float radius, float dt) {
                x += vx * dt;
                y += vy * dt;
                const float dx = x - CENTRE, dy = y - CENTRE, dist = sqrtf(dx * dx + dy * dy);
                const float limit = PLAYFIELD_RADIUS - radius;
                if (dist > limit && dist > 0.001f) {
                    const float nx = dx / dist, ny = dy / dist, dot = vx * nx + vy * ny;
                    vx -= 2 * dot * nx;
                    vy -= 2 * dot * ny;
                    x -= nx * (dist - limit);
                    y -= ny * (dist - limit);
                }
            }

        public:
            uint16_t defaultColor() const override { return 0xFD20; }

            void seed(uint32_t now) override {
                for (int i = 0; i < STARS; i++) {
                    sx[i] = random(0, SIZE);
                    sy[i] = random(0, SIZE);
                    tier[i] = random(0, 3);
                }
                const float d = randAngle();
                driftX = cosf(d);
                driftY = sinf(d);
                blink = -1;
                blinkUntil = 0;
                nextBlink = now + random(500, 2500);
                const float pa = randAngle(), pd = randAngle();
                px = CENTRE + 30 * cosf(pa);
                py = CENTRE + 30 * sinf(pa);
                pvx = 14 * cosf(pd);
                pvy = 14 * sinf(pd);
                for (int r = 0; r < ROCKS; r++) {
                    const float a = randAngle(), dist = random(30, PLAYFIELD_RADIUS - 10), dir = randAngle(), speed = random(18, 34);
                    rx[r] = CENTRE + dist * cosf(a);
                    ry[r] = CENTRE + dist * sinf(a);
                    rvx[r] = speed * cosf(dir);
                    rvy[r] = speed * sinf(dir);
                    const int base = random(6, 10);
                    int maxOff = 0;
                    for (int p = 0; p < POINTS; p++) {
                        const float pa2 = 360.0f / POINTS * p * DEG_TO_RAD;
                        const int pr = base + random(-3, 4);
                        ox[r][p] = lroundf(pr * cosf(pa2));
                        oy[r][p] = lroundf(pr * sinf(pa2));
                        maxOff = max(maxOff, max(abs(ox[r][p]), abs(oy[r][p])));
                    }
                    bound[r] = maxOff + 1;
                }
            }

            void step(float dt, uint32_t now) override {
                if (blink >= 0 && now >= blinkUntil) {
                    blink = -1;
                    nextBlink = now + random(500, 2500);
                } else if (blink < 0 && now >= nextBlink) {
                    blink = random(0, STARS); // one star twinkles for a moment
                    blinkUntil = now + random(120, 300);
                }
                static const float SPEED[] = {1.2f, 0.8f, 0.5f};
                for (int i = 0; i < STARS; i++) {
                    sx[i] = fmodf(sx[i] + driftX * SPEED[tier[i]] * dt + SIZE, SIZE);
                    sy[i] = fmodf(sy[i] + driftY * SPEED[tier[i]] * dt + SIZE, SIZE);
                }
                bounce(px, py, pvx, pvy, PLANET_R, dt);
                for (int r = 0; r < ROCKS; r++) bounce(rx[r], ry[r], rvx[r], rvy[r], bound[r], dt);
            }

            void paint(LGFX_Sprite &spr, uint16_t color) override {
                static const uint16_t STAR[] = {TFT_WHITE, LIGHTGREY, DARKGREY};
                spr.fillScreen(TFT_BLACK);
                for (int i = 0; i < STARS; i++) {
                    const uint16_t c = i == blink ? (tier[i] == 2 ? TFT_WHITE : DARKGREY) : STAR[tier[i]];
                    spr.drawPixel((int) sx[i], (int) sy[i], c);
                }
                const int x = px, y = py;
                spr.fillCircle(x, y, PLANET_R, color);
                spr.drawCircle(x, y, PLANET_R - 7, SILVER);
                // Label in black or white, whichever reads on the planet's colour.
                const int r8 = ((color >> 11) & 0x1F) << 3, g8 = ((color >> 5) & 0x3F) << 2, b8 = (color & 0x1F) << 3;
                const uint16_t label = (r8 * 299 + g8 * 587 + b8 * 114) / 1000 < 110 ? TFT_WHITE : TFT_BLACK;
                text::draw(spr, "Orb-It", x, y, 13, Align::MiddleCenter, label, color);
                for (int r = 0; r < ROCKS; r++) {
                    for (int p = 0; p < POINTS; p++) {
                        const int q = (p + 1) % POINTS;
                        spr.drawLine(rx[r] + ox[r][p], ry[r] + oy[r][p], rx[r] + ox[r][q], ry[r] + oy[r][q], LIGHTGREY);
                    }
                }
            }
        };

        // ================= Matrix =================
        // 5x7 katakana and digits, drawn at 2x.
        const uint8_t GLYPHS[][7] = {
            {0x1F, 0x01, 0x05, 0x06, 0x04, 0x04, 0x08}, {0x01, 0x02, 0x04, 0x0C, 0x14, 0x04, 0x04}, {0x04, 0x1F, 0x11, 0x01, 0x02, 0x04, 0x08},
            {0x00, 0x1F, 0x04, 0x04, 0x04, 0x1F, 0x00}, {0x02, 0x1F, 0x02, 0x06, 0x0A, 0x12, 0x02}, {0x08, 0x1F, 0x09, 0x09, 0x09, 0x09, 0x12},
            {0x04, 0x1F, 0x04, 0x1F, 0x04, 0x04, 0x04}, {0x0F, 0x09, 0x11, 0x01, 0x02, 0x04, 0x08}, {0x08, 0x0F, 0x12, 0x02, 0x02, 0x02, 0x04},
            {0x00, 0x1F, 0x01, 0x01, 0x01, 0x1F, 0x00}, {0x0A, 0x1F, 0x0A, 0x0A, 0x02, 0x04, 0x08}, {0x00, 0x19, 0x01, 0x19, 0x01, 0x02, 0x1C},
            {0x1F, 0x01, 0x02, 0x04, 0x0A, 0x11, 0x00}, {0x08, 0x1F, 0x09, 0x0A, 0x08, 0x08, 0x07}, {0x11, 0x11, 0x09, 0x01, 0x02, 0x04, 0x08},
            {0x0F, 0x09, 0x15, 0x03, 0x02, 0x04, 0x08}, {0x02, 0x1C, 0x04, 0x1F, 0x04, 0x04, 0x08}, {0x15, 0x15, 0x15, 0x01, 0x02, 0x04, 0x08},
            {0x0E, 0x00, 0x1F, 0x04, 0x04, 0x04, 0x08}, {0x08, 0x08, 0x08, 0x0C, 0x0A, 0x08, 0x08}, {0x04, 0x04, 0x1F, 0x04, 0x04, 0x08, 0x10},
            {0x00, 0x0E, 0x00, 0x00, 0x00, 0x1F, 0x00}, {0x1F, 0x01, 0x0A, 0x04, 0x0A, 0x10, 0x00}, {0x04, 0x1F, 0x02, 0x04, 0x0E, 0x15, 0x04},
            {0x02, 0x02, 0x02, 0x04, 0x04, 0x08, 0x10}, {0x00, 0x0A, 0x09, 0x09, 0x11, 0x11, 0x00}, {0x10, 0x10, 0x1F, 0x10, 0x10, 0x10, 0x0F},
            {0x1F, 0x01, 0x01, 0x01, 0x02, 0x04, 0x08}, {0x00, 0x08, 0x14, 0x02, 0x01, 0x00, 0x00}, {0x04, 0x1F, 0x04, 0x15, 0x15, 0x04, 0x04},
            {0x1F, 0x01, 0x01, 0x0A, 0x04, 0x02, 0x00}, {0x0E, 0x01, 0x0E, 0x01, 0x1E, 0x01, 0x00}, {0x04, 0x04, 0x08, 0x08, 0x12, 0x1F, 0x01},
            {0x01, 0x01, 0x0A, 0x04, 0x0A, 0x10, 0x00}, {0x0E, 0x04, 0x1F, 0x04, 0x04, 0x04, 0x03}, {0x08, 0x1F, 0x09, 0x0A, 0x08, 0x08, 0x08},
            {0x00, 0x0E, 0x02, 0x02, 0x02, 0x1F, 0x00}, {0x1F, 0x01, 0x1F, 0x01, 0x01, 0x1F, 0x00}, {0x0E, 0x00, 0x1F, 0x01, 0x01, 0x02, 0x0C},
            {0x11, 0x11, 0x11, 0x01, 0x01, 0x02, 0x04}, {0x1F, 0x11, 0x11, 0x01, 0x02, 0x04, 0x08}, {0x1F, 0x01, 0x1F, 0x01, 0x02, 0x04, 0x08},
            {0x00, 0x19, 0x01, 0x01, 0x02, 0x04, 0x18}, {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
            {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
            {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
            {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
        };
        const int NUM_GLYPHS = sizeof(GLYPHS) / sizeof(GLYPHS[0]);

        class Matrix : public ScreensaverControl::Effect {
            static const int COLS = 20, ROWS = 15, CELL_W = SIZE / COLS, CELL_H = SIZE / ROWS;
            uint8_t glyph[COLS][ROWS];
            int8_t head[COLS];
            uint8_t length[COLS];
            uint16_t stepMs[COLS];
            uint32_t nextStep[COLS];

            void schedule(int c, uint32_t now, int maxDelay) {
                head[c] = -1;
                length[c] = random(5, 14);
                stepMs[c] = random(60, 171); // each drop falls at its own speed
                nextStep[c] = now + random(0, maxDelay);
            }

            static bool visible(int col, int row) {
                const int l = col * CELL_W, t = row * CELL_H;
                const int dx = max(max(l - CENTRE, CENTRE - (l + CELL_W)), 0);
                const int dy = max(max(t - CENTRE, CENTRE - (t + CELL_H)), 0);
                return dx * dx + dy * dy <= CENTRE * CENTRE;
            }

            void drawGlyph(LGFX_Sprite &spr, int col, int row, uint16_t color) {
                if (!visible(col, row)) return;
                const uint8_t *rows = GLYPHS[glyph[col][row]];
                const int left = col * CELL_W + (CELL_W - 10) / 2, top = row * CELL_H + (CELL_H - 14) / 2;
                for (int y = 0; y < 7; y++) {
                    for (int x = 0; x < 5; x++) {
                        if (rows[y] & (1 << x)) spr.fillRect(left + x * 2, top + y * 2, 2, 2, color);
                    }
                }
            }

        public:
            uint16_t defaultColor() const override { return TFT_GREEN; }

            void seed(uint32_t now) override {
                for (int c = 0; c < COLS; c++) schedule(c, now, 1250);
            }

            void step(float, uint32_t now) override {
                for (int c = 0; c < COLS; c++) {
                    if ((int32_t) (now - nextStep[c]) < 0) continue;
                    const int h = ++head[c];
                    if (h >= 0 && h < ROWS) glyph[c][h] = random(0, NUM_GLYPHS);
                    if (h - length[c] >= ROWS - 1) schedule(c, now, 2500); // tail has left the screen
                    else nextStep[c] = now + stepMs[c];
                }
                // Now and then a glyph in a falling column changes.
                if (random(0, 3) == 0) {
                    const int c = random(0, COLS);
                    const int first = max(head[c] - length[c] + 3, 0), last = min(head[c] - 2, ROWS - 1);
                    if (head[c] >= 0 && first <= last) glyph[c][random(first, last + 1)] = random(0, NUM_GLYPHS);
                }
            }

            void paint(LGFX_Sprite &spr, uint16_t color) override {
                spr.fillScreen(TFT_BLACK);
                const uint16_t fade1 = dim(color, 150), fade2 = dim(color, 70);
                for (int c = 0; c < COLS; c++) {
                    if (head[c] < 0) continue;
                    for (int r = max(head[c] - length[c] + 1, 0); r <= min((int) head[c], ROWS - 1); r++) {
                        const int fromEnd = r - (head[c] - length[c]);
                        const uint16_t col = r == head[c] ? TFT_WHITE : fromEnd == 1 ? fade2 : fromEnd == 2 ? fade1 : color;
                        drawGlyph(spr, c, r, col);
                    }
                }
            }
        };

        // ================= Warp =================
        class Warp : public ScreensaverControl::Effect {
            static const int STARS = 40, RINGS = 4, EDGE = 122;
            float bendX, bendY, targetX, targetY, turnTimer;
            int8_t dirX[STARS], dirY[STARS], sBendX[STARS], sBendY[STARS];
            float radius[STARS];
            float ringR[RINGS];
            int8_t rBendX[RINGS], rBendY[RINGS];

            static float speedAt(float r) { return 10 + r * 1.5f; }
            // The bend fades out towards the edge: the far end of the tunnel swings
            // off while what's about to pass stays centred.
            static int bendAt(int8_t bend, int r) { return r >= EDGE ? 0 : bend * (EDGE - r) / EDGE; }

            void newTurn() {
                const float a = randAngle(), size = random(20, 56);
                targetX = size * cosf(a);
                targetY = size * sinf(a);
                turnTimer = random(2000, 6000) / 1000.0f;
            }

            void spawn(int i, int minR, int maxR) {
                const float a = randAngle();
                dirX[i] = lroundf(127 * cosf(a));
                dirY[i] = lroundf(127 * sinf(a));
                sBendX[i] = lroundf(bendX);
                sBendY[i] = lroundf(bendY);
                radius[i] = random(minR, maxR);
            }

        public:
            uint16_t defaultColor() const override { return 0x867D; }

            void seed(uint32_t) override {
                newTurn();
                bendX = targetX;
                bendY = targetY;
                for (int i = 0; i < STARS; i++) spawn(i, 3, PLAYFIELD_RADIUS);
                static const float START[] = {2, 10, 27, 59};
                for (int r = 0; r < RINGS; r++) {
                    ringR[r] = START[r];
                    rBendX[r] = lroundf(bendX);
                    rBendY[r] = lroundf(bendY);
                }
            }

            void step(float dt, uint32_t) override {
                if ((turnTimer -= dt) <= 0) newTurn();
                const float swing = min(dt * 1.5f, 1.0f);
                bendX += (targetX - bendX) * swing;
                bendY += (targetY - bendY) * swing;
                for (int r = 0; r < RINGS; r++) {
                    ringR[r] += speedAt(ringR[r]) * dt;
                    if (ringR[r] > EDGE) {
                        ringR[r] = 2;
                        rBendX[r] = lroundf(bendX);
                        rBendY[r] = lroundf(bendY);
                    }
                }
                for (int i = 0; i < STARS; i++) {
                    radius[i] += speedAt(radius[i]) * dt;
                    if (radius[i] - speedAt(radius[i]) * 0.06f > EDGE) spawn(i, 3, 25);
                }
            }

            void paint(LGFX_Sprite &spr, uint16_t color) override {
                spr.fillScreen(TFT_BLACK);
                for (int r = 0; r < RINGS; r++) {
                    const int rad = ringR[r];
                    spr.drawCircle(CENTRE + bendAt(rBendX[r], rad), CENTRE + bendAt(rBendY[r], rad), rad, dim(color, min(45 + rad * 2, 255)));
                }
                for (int i = 0; i < STARS; i++) {
                    // A streak, longer and brighter the faster it's going.
                    const int outer = constrain(radius[i], 0.0f, 255.0f);
                    const int inner = constrain(radius[i] - speedAt(radius[i]) * 0.06f, 0.0f, 255.0f);
                    const int x1 = CENTRE + bendAt(sBendX[i], inner) + dirX[i] * inner / 127;
                    const int y1 = CENTRE + bendAt(sBendY[i], inner) + dirY[i] * inner / 127;
                    const int x2 = CENTRE + bendAt(sBendX[i], outer) + dirX[i] * outer / 127;
                    const int y2 = CENTRE + bendAt(sBendY[i], outer) + dirY[i] * outer / 127;
                    spr.drawLine(x1, y1, x2, y2, dim(TFT_WHITE, min(70 + outer * 2, 255)));
                }
            }
        };

        // ================= Orrery =================
        class Orrery : public ScreensaverControl::Effect {
            struct Planet {
                int orbit, radius;
                uint16_t color;
                float speed; // radians per second
                bool ringed;
            };
            static constexpr int PLANETS = 5, MOON_PLANET = 2, STARS = 25;
            const Planet P[PLANETS] = {
                {26, 3, SILVER, 0.785f, false},  {38, 4, 0xFDA0, 0.444f, false}, {57, 5, 0x3D9F, 0.242f, false},
                {81, 7, 0xE5F0, 0.143f, true},   {99, 4, TFT_CYAN, 0.106f, false},
            };
            float angle[PLANETS + 1]; // the last is the moon's, round its planet
            uint8_t starX[STARS], starY[STARS], starTier[STARS];

        public:
            uint16_t defaultColor() const override { return 0xFEA0; }

            void seed(uint32_t) override {
                for (float &a : angle) a = randAngle();
                for (int i = 0; i < STARS; i++) {
                    const float a = randAngle(), d = random(18, CENTRE - 2);
                    starX[i] = CENTRE + d * cosf(a);
                    starY[i] = CENTRE + d * sinf(a);
                    starTier[i] = random(0, 2);
                }
            }

            void step(float dt, uint32_t) override {
                for (int b = 0; b <= PLANETS; b++) angle[b] = fmodf(angle[b] + (b == PLANETS ? 2.1f : P[b].speed) * dt, TWO_PI);
            }

            void paint(LGFX_Sprite &spr, uint16_t sun) override {
                spr.fillScreen(TFT_BLACK);
                for (const Planet &p : P) spr.drawCircle(CENTRE, CENTRE, p.orbit, 0x39E7);
                for (int i = 0; i < STARS; i++) spr.drawPixel(starX[i], starY[i], starTier[i] ? DARKGREY : LIGHTGREY);
                spr.fillCircle(CENTRE, CENTRE, 15, dim(sun, 90)); // glow
                spr.fillCircle(CENTRE, CENTRE, 13, sun);
                for (int b = 0; b < PLANETS; b++) {
                    const int x = lroundf(CENTRE + P[b].orbit * cosf(angle[b])), y = lroundf(CENTRE + P[b].orbit * sinf(angle[b]));
                    spr.fillCircle(x, y, P[b].radius, P[b].color);
                    if (P[b].ringed) spr.drawLine(x - 10, y + 3, x + 10, y - 3, LIGHTGREY);
                    if (b == MOON_PLANET) {
                        spr.fillCircle(lroundf(x + 9 * cosf(angle[PLANETS])), lroundf(y + 9 * sinf(angle[PLANETS])), 1, LIGHTGREY);
                    }
                }
            }
        };

        // ================= Radar =================
        // Angles here are TFT_eSPI's (0 = 6 o'clock, clockwise), as in info-orbs.
        class Radar : public ScreensaverControl::Effect {
            static const int SCOPE = 105, SECTOR_DEG = 3, SECTORS = 360 / SECTOR_DEG, BLIPS = 6;
            static const int BAND = 4, BANDS = 5, GONE = 80;
            int beam;
            float stepsDue;
            uint8_t blipSector[BLIPS], blipRadius[BLIPS], blipAge[BLIPS];

            void place(int b) {
                blipSector[b] = random(0, SECTORS);
                blipRadius[b] = random(20, 99);
            }
            static int behind(int sector, int steps) { return ((sector - steps) % SECTORS + SECTORS) % SECTORS; }
            static uint8_t blipBrightness(int age) {
                return age < 20 ? 255 : age < 40 ? 170 : age < 60 ? 110 : age < GONE ? 60 : 0;
            }

            void advance() {
                beam = (beam + 1) % SECTORS;
                for (int b = 0; b < BLIPS; b++) {
                    const bool wasGone = blipAge[b] >= GONE;
                    if (behind(beam, blipSector[b]) == 0) blipAge[b] = 0; // lit as the beam passes
                    else if (!wasGone) blipAge[b]++;
                    if (blipAge[b] >= GONE && !wasGone) {
                        // Faded out: by the next pass it has moved a little, or jumped.
                        if (random(0, 4) == 0) {
                            place(b);
                        } else {
                            blipSector[b] = behind(blipSector[b], random(-2, 3));
                            blipRadius[b] = constrain(blipRadius[b] + random(-6, 7), 20, 98);
                        }
                    }
                }
            }

        public:
            uint16_t defaultColor() const override { return TFT_GREEN; }

            void seed(uint32_t) override {
                beam = random(0, SECTORS);
                stepsDue = 0;
                for (int b = 0; b < BLIPS; b++) {
                    place(b);
                    blipAge[b] = GONE;
                }
            }

            void step(float dt, uint32_t) override {
                stepsDue += dt * 20; // one sweep every 6 seconds
                while (stepsDue >= 1) {
                    stepsDue -= 1;
                    advance();
                }
            }

            void paint(LGFX_Sprite &spr, uint16_t color) override {
                static const uint8_t AFTERGLOW[] = {130, 95, 65, 42, 25};
                spr.fillScreen(TFT_BLACK);
                for (int age = BAND * BANDS; age >= 0; age--) {
                    const uint8_t level = age == 0 ? 255 : AFTERGLOW[(age - 1) / BAND];
                    const int s = behind(beam, age);
                    // A 3° slice is drawn as a triangle from the centre: the
                    // same to the eye, and far quicker than a pie-slice arc.
                    const float a0 = s * SECTOR_DEG * DEG_TO_RAD, a1 = (s + 1) * SECTOR_DEG * DEG_TO_RAD;
                    spr.fillTriangle(CENTRE, CENTRE, CENTRE - lroundf(SCOPE * sinf(a0)), CENTRE + lroundf(SCOPE * cosf(a0)),
                                     CENTRE - lroundf(SCOPE * sinf(a1)), CENTRE + lroundf(SCOPE * cosf(a1)), dim(color, level));
                }
                const uint16_t overlay = dim(color, 90);
                for (int r : {35, 70, SCOPE}) spr.drawCircle(CENTRE, CENTRE, r - 1, overlay);
                spr.drawFastVLine(CENTRE, CENTRE - SCOPE, 2 * SCOPE + 1, overlay);
                spr.drawFastHLine(CENTRE - SCOPE, CENTRE, 2 * SCOPE + 1, overlay);
                for (int b = 0; b < BLIPS; b++) {
                    if (blipAge[b] >= GONE) continue;
                    const float a = (blipSector[b] + 0.5f) * SECTOR_DEG * DEG_TO_RAD;
                    spr.fillCircle(CENTRE - lroundf(blipRadius[b] * sinf(a)), CENTRE + lroundf(blipRadius[b] * cosf(a)), 3,
                                   dim(color, blipBrightness(blipAge[b])));
                }
                spr.fillCircle(CENTRE, CENTRE, 3, color);
            }
        };

        const char *const PARAM_EFFECTS[] = {"asteroids", "matrix", "warp", "orrery", "radar", "cycle", nullptr};

        int effectIndex(const char *name) {
            for (int i = 0; ScreensaverControl::EFFECTS[i]; i++) {
                if (strcmp(name, ScreensaverControl::EFFECTS[i]) == 0) return i;
            }
            return 0;
        }
    }

    const char *const ScreensaverControl::EFFECTS[] = {"asteroids", "matrix", "warp", "orrery", "radar", nullptr};

    ScreensaverControl::ScreensaverControl() {
        for (Screen &s : screens) {
            s.effects[0] = new Asteroids();
            s.effects[1] = new Matrix();
            s.effects[2] = new Warp();
            s.effects[3] = new Orrery();
            s.effects[4] = new Radar();
        }
    }

    ScreensaverControl::~ScreensaverControl() {
        for (Screen &s : screens) {
            for (Effect *e : s.effects) delete e;
        }
    }

    int ScreensaverControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        if (int status = optionalChoice(in, out, "effect", PARAM_EFFECTS, error)) {
            error = "unknown screensaver effect '" + in["effect"].as<String>() + "'";
            return status;
        }
        if (int status = colorIfSet(in, out, "color", error)) return status;
        if (strcmp(out["effect"], "cycle") == 0) {
            JsonVariantConst v = in["cycleSeconds"];
            if (!v.isNull() && !v.is<long>()) {
                error = "params.cycleSeconds must be a whole number";
                return 400;
            }
            out["cycleSeconds"] = constrain(v.isNull() ? 300L : v.as<long>(), 30L, 86400L);
        }
        return 0;
    }

    AfterWrite ScreensaverControl::applied(int screen, JsonObjectConst, JsonObjectConst) {
        screens[screen].fresh = true; // any write starts the animation afresh
        return AfterWrite::Keep;
    }

    void ScreensaverControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &ctx) {
        Screen &s = screens[ctx.screen];
        const uint32_t now = ctx.nowMs;
        const char *effect = p["effect"] | "asteroids";

        int want;
        if (strcmp(effect, "cycle") == 0) {
            // Start from the first effect whenever cycling begins; move on
            // after cycleSeconds.
            if (s.fresh || s.showing < 0) {
                want = 0;
                s.effectStarted = now;
            } else if (now - s.effectStarted >= (uint32_t) p["cycleSeconds"].as<long>() * 1000UL) {
                want = (s.showing + 1) % 5;
                s.effectStarted = now;
            } else {
                want = s.showing;
            }
        } else {
            want = effectIndex(effect);
        }

        Effect *e = s.effects[want];
        if (s.fresh || want != s.showing) {
            s.showing = want;
            s.fresh = false;
            e->seed(now);
        } else {
            e->step(min((now - s.lastFrame) / 1000.0f, MAX_DT), now);
        }
        s.lastFrame = now;
        e->paint(spr, p["color"].isNull() ? e->defaultColor() : p["color"].as<uint16_t>());
    }

}

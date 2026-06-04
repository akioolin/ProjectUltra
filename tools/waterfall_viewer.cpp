// waterfall_viewer — debug spectrogram/waterfall of a recorded OTASim exchange.
//
// Loads one or more 48 kHz mono WAVs (e.g. the OTASim capture's per-station
// _tx_48k_f32.wav files), sums them (half-duplex → the complete over-the-air
// timeline), computes a Hann-windowed FFT spectrogram, and either:
//   --png <out>   : renders the whole exchange to a PNG (headless) and exits
//   (default)     : opens an interactive SDL2 window (pan/zoom/hover readout)
//
// Reuses ultra::FFT (PocketFFT) and ultra::tools::io WAV loader.
//
// Usage:
//   waterfall_viewer A_tx_48k_f32.wav B_tx_48k_f32.wav [--png out.png]
//                    [--fft 1024] [--hop 256] [--fmax 4000]
//                    [--db-min -90] [--db-max -20]

#include "io/wav_io.hpp"
#include "ultra/dsp.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#ifndef WATERFALL_NO_SDL
#include <SDL.h>
#endif

namespace {

struct Spectrogram {
    int cols = 0;           // time columns
    int bins = 0;           // freq bins (0..Nyquist), = fft/2+1
    float bin_hz = 0.0f;    // Hz per bin
    float col_sec = 0.0f;   // seconds per column (hop / sr)
    std::vector<float> db;  // [col * bins + bin], dB
    float db_lo = -120.0f, db_hi = 0.0f;
};

Spectrogram computeSpectrogram(const std::vector<float>& x, int fft_size, int hop,
                               float sample_rate) {
    Spectrogram s;
    s.bins = fft_size / 2 + 1;
    s.bin_hz = sample_rate / static_cast<float>(fft_size);
    s.col_sec = static_cast<float>(hop) / sample_rate;
    if (static_cast<int>(x.size()) < fft_size) {
        return s;
    }
    s.cols = 1 + (static_cast<int>(x.size()) - fft_size) / hop;

    // Hann window.
    std::vector<float> win(fft_size);
    for (int i = 0; i < fft_size; ++i) {
        win[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i /
                                          (fft_size - 1)));
    }

    ultra::FFT fft(static_cast<size_t>(fft_size));
    std::vector<float> frame(fft_size);
    std::vector<ultra::Complex> spec(s.bins);
    s.db.assign(static_cast<size_t>(s.cols) * s.bins, -200.0f);

    const float norm = 2.0f / fft_size;
    float lo = 1e30f, hi = -1e30f;
    for (int c = 0; c < s.cols; ++c) {
        const int start = c * hop;
        for (int i = 0; i < fft_size; ++i) frame[i] = x[start + i] * win[i];
        fft.forwardReal(frame.data(), spec.data());
        for (int b = 0; b < s.bins; ++b) {
            const float mag = std::abs(spec[b]) * norm;
            const float d = 20.0f * std::log10(mag + 1e-9f);
            s.db[static_cast<size_t>(c) * s.bins + b] = d;
            if (d > hi) hi = d;
            if (d < lo && d > -180.0f) lo = d;
        }
    }
    s.db_lo = lo;
    s.db_hi = hi;
    return s;
}

// Heat-ish colormap: black→blue→cyan→green→yellow→red→white. t in [0,1].
void colormap(float t, uint8_t& r, uint8_t& g, uint8_t& b) {
    t = std::clamp(t, 0.0f, 1.0f);
    struct Stop { float p; float r, g, b; };
    static const Stop stops[] = {
        {0.00f, 0, 0, 0},      {0.15f, 0, 0, 160},    {0.35f, 0, 180, 220},
        {0.55f, 0, 200, 0},    {0.72f, 230, 230, 0},  {0.88f, 240, 60, 0},
        {1.00f, 255, 255, 255}};
    for (int i = 1; i < 7; ++i) {
        if (t <= stops[i].p) {
            const float f = (t - stops[i - 1].p) / (stops[i].p - stops[i - 1].p);
            r = static_cast<uint8_t>(stops[i - 1].r + f * (stops[i].r - stops[i - 1].r));
            g = static_cast<uint8_t>(stops[i - 1].g + f * (stops[i].g - stops[i - 1].g));
            b = static_cast<uint8_t>(stops[i - 1].b + f * (stops[i].b - stops[i - 1].b));
            return;
        }
    }
    r = g = b = 255;
}

float dbNorm(float d, float db_min, float db_max) {
    return (d - db_min) / (db_max - db_min);
}

// ---- PNG mode: render whole exchange (time→X, freq→Y, 0 Hz at bottom) ----
int renderPng(const Spectrogram& s, float fmax, float db_min, float db_max,
              int target_w, int freq_scale, const std::string& out) {
    const int used_bins = std::min(s.bins, static_cast<int>(fmax / s.bin_hz) + 1);
    // Max-pool time columns down to <= target_w so the PNG is viewable.
    const int pool = std::max(1, (s.cols + target_w - 1) / target_w);
    const int W = (s.cols + pool - 1) / pool;
    const int H = used_bins * freq_scale;
    std::vector<uint8_t> img(static_cast<size_t>(W) * H * 3, 0);

    for (int px = 0; px < W; ++px) {
        for (int b = 0; b < used_bins; ++b) {
            float best = -200.0f;  // max-pool (peak hold) over the pooled columns
            for (int k = 0; k < pool; ++k) {
                const int c = px * pool + k;
                if (c >= s.cols) break;
                best = std::max(best, s.db[static_cast<size_t>(c) * s.bins + b]);
            }
            uint8_t r, g, bl;
            colormap(dbNorm(best, db_min, db_max), r, g, bl);
            for (int fs = 0; fs < freq_scale; ++fs) {
                const int y = H - 1 - (b * freq_scale + fs);  // 0 Hz at bottom
                uint8_t* p = &img[(static_cast<size_t>(y) * W + px) * 3];
                p[0] = r; p[1] = g; p[2] = bl;
            }
        }
    }
    // 1 kHz horizontal gridlines (faint).
    for (float f = 1000.0f; f < fmax; f += 1000.0f) {
        const int y = H - 1 - static_cast<int>((f / s.bin_hz) * freq_scale);
        if (y < 0 || y >= H) continue;
        for (int px = 0; px < W; ++px) {
            uint8_t* p = &img[(static_cast<size_t>(y) * W + px) * 3];
            p[0] = std::min(255, p[0] + 50); p[1] = std::min(255, p[1] + 50);
            p[2] = std::min(255, p[2] + 50);
        }
    }
    if (!stbi_write_png(out.c_str(), W, H, 3, img.data(), W * 3)) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s  (%dx%d, %.1fs, %d cols pooled x%d, 0-%.0f Hz, dB[%.0f,%.0f])\n",
                out.c_str(), W, H, s.cols * s.col_sec, s.cols, pool, fmax, db_min, db_max);
    return 0;
}

#ifndef WATERFALL_NO_SDL
// ---- Interactive SDL2 mode ----
int runInteractive(const Spectrogram& s, float fmax, float db_min, float db_max) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const int W = 1280, H = 700;
    SDL_Window* win = SDL_CreateWindow("ultra waterfall — pan: ←→ / drag · zoom: +/- / wheel · q quit",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture* tex = nullptr;
    int tw = 0, th = 0;

    const int used_bins = std::min(s.bins, static_cast<int>(fmax / s.bin_hz) + 1);
    double view_col0 = 0.0;                 // leftmost spectrogram column
    double cols_per_screen = s.cols;        // zoom: columns across the window
    std::vector<uint32_t> pixels;

    bool running = true, dragging = false;
    int win_w = W, win_h = H;
    int mouse_x = 0, mouse_y = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = e.window.data1; win_h = e.window.data2;
            } else if (e.type == SDL_KEYDOWN) {
                const double pan = cols_per_screen * 0.1;
                switch (e.key.keysym.sym) {
                    case SDLK_q: case SDLK_ESCAPE: running = false; break;
                    case SDLK_LEFT:  view_col0 -= pan; break;
                    case SDLK_RIGHT: view_col0 += pan; break;
                    case SDLK_EQUALS: case SDLK_PLUS:  cols_per_screen *= 0.8; break;
                    case SDLK_MINUS: cols_per_screen /= 0.8; break;
                    case SDLK_0: view_col0 = 0; cols_per_screen = s.cols; break;
                    default: break;
                }
            } else if (e.type == SDL_MOUSEWHEEL) {
                const double f = (e.wheel.y > 0) ? 0.85 : 1.0 / 0.85;
                const double anchor = view_col0 + (double)mouse_x / win_w * cols_per_screen;
                cols_per_screen *= f;
                view_col0 = anchor - (double)mouse_x / win_w * cols_per_screen;
            } else if (e.type == SDL_MOUSEBUTTONDOWN) dragging = true;
            else if (e.type == SDL_MOUSEBUTTONUP) dragging = false;
            else if (e.type == SDL_MOUSEMOTION) {
                mouse_x = e.motion.x; mouse_y = e.motion.y;
                if (dragging) view_col0 -= (double)e.motion.xrel / win_w * cols_per_screen;
            }
        }
        cols_per_screen = std::clamp(cols_per_screen, 16.0, (double)s.cols);
        view_col0 = std::clamp(view_col0, 0.0, std::max(0.0, s.cols - cols_per_screen));

        // Build a window-sized texture from the visible region (CPU blit).
        if (!tex || tw != win_w || th != win_h) {
            if (tex) SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, win_w, win_h);
            tw = win_w; th = win_h;
            pixels.resize((size_t)win_w * win_h);
        }
        for (int py = 0; py < win_h; ++py) {
            const float frac = 1.0f - (float)py / win_h;          // top=high freq
            const int b = std::min(used_bins - 1, (int)(frac * used_bins));
            for (int px = 0; px < win_w; ++px) {
                const int c = (int)(view_col0 + (double)px / win_w * cols_per_screen);
                float d = -200.0f;
                if (c >= 0 && c < s.cols) d = s.db[(size_t)c * s.bins + b];
                uint8_t r, g, bl; colormap(dbNorm(d, db_min, db_max), r, g, bl);
                pixels[(size_t)py * win_w + px] = (0xFFu << 24) | (r << 16) | (g << 8) | bl;
            }
        }
        SDL_UpdateTexture(tex, nullptr, pixels.data(), win_w * 4);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);

        // Live readout in the title (time + freq under cursor + view span).
        const double t0 = view_col0 * s.col_sec;
        const double t1 = (view_col0 + cols_per_screen) * s.col_sec;
        const double cur_t = (view_col0 + (double)mouse_x / win_w * cols_per_screen) * s.col_sec;
        const double cur_f = (1.0 - (double)mouse_y / win_h) * fmax;
        char title[256];
        std::snprintf(title, sizeof(title),
                      "ultra waterfall — view %.1f-%.1fs (%.2fs/screen) · cursor t=%.2fs f=%.0fHz",
                      t0, t1, t1 - t0, cur_t, cur_f);
        SDL_SetWindowTitle(win, title);
    }
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
#endif

// ============================ automated analysis ============================

// 5x7 bitmap font (bits 4..0 = left..right pixel). Closed label vocabulary.
struct Glyph { char c; uint8_t r[7]; };
static const Glyph kFont[] = {
    {' ',{0,0,0,0,0,0,0}},
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}, {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}}, {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}}, {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W',{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}}, {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'.',{0,0,0,0,0,0x0C,0x0C}}, {'-',{0,0,0,0x1F,0,0,0}}, {':',{0,0,0x04,0,0,0x04,0}},
    {'>',{0x10,0x08,0x04,0x02,0x04,0x08,0x10}}, {'/',{0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
    {'(',{0x02,0x04,0x08,0x08,0x08,0x04,0x02}}, {')',{0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
    {'%',{0x19,0x1A,0x04,0x0B,0x13,0,0}},
};
const uint8_t* glyphFor(char c) {
    for (const auto& g : kFont) if (g.c == c) return g.r;
    return kFont[0].r;
}
void drawText(uint8_t* img, int W, int H, int x, int y, int sc, const std::string& t,
              uint8_t r, uint8_t g, uint8_t b) {
    int cx = x;
    for (char ch : t) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const uint8_t* gl = glyphFor(ch);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (gl[row] & (1 << (4 - col)))
                    for (int dy = 0; dy < sc; ++dy)
                        for (int dx = 0; dx < sc; ++dx) {
                            int px = cx + col * sc + dx, py = y + row * sc + dy;
                            if (px >= 0 && px < W && py >= 0 && py < H) {
                                uint8_t* p = &img[(static_cast<size_t>(py) * W + px) * 3];
                                p[0] = r; p[1] = g; p[2] = b;
                            }
                        }
        cx += 6 * sc;
    }
}
void fillRect(uint8_t* img, int W, int H, int x0, int y0, int x1, int y1,
              uint8_t r, uint8_t g, uint8_t b, float alpha = 1.0f) {
    for (int y = std::max(0, y0); y < std::min(H, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(W, x1); ++x) {
            uint8_t* p = &img[(static_cast<size_t>(y) * W + x) * 3];
            p[0] = static_cast<uint8_t>(p[0] * (1 - alpha) + r * alpha);
            p[1] = static_cast<uint8_t>(p[1] * (1 - alpha) + g * alpha);
            p[2] = static_cast<uint8_t>(p[2] * (1 - alpha) + b * alpha);
        }
}

enum class Kind { Setup, Anchor, Data, Ack, Other };
struct Segment {
    int c0, c1;          // spectrogram columns
    Kind kind;
    std::string label;   // short on-image label
    std::string tx;      // transmitting station
    float bw;            // mean fractional bandwidth
};

void kindColor(Kind k, uint8_t& r, uint8_t& g, uint8_t& b) {
    switch (k) {
        case Kind::Setup:  r=180; g=90;  b=210; break;  // purple — MC-DPSK handshake
        case Kind::Anchor: r=0;   g=210; b=230; break;  // cyan — chirp+LTS re-acquire
        case Kind::Data:   r=70;  g=200; b=80;  break;  // green — OFDM payload
        case Kind::Ack:    r=240; g=120; b=0;   break;  // orange — tone-burst ACK
        default:           r=120; g=120; b=120; break;
    }
}

// Segment the exchange from energy + bandwidth; attribute direction from the two
// _rx files (a transmission shows up in the OTHER station's _rx). files: name+samples.
std::vector<Segment> analyzeExchange(
    const std::vector<std::pair<std::string, std::vector<float>>>& files,
    const Spectrogram& s, int fft_size, int hop) {
    const int nf = static_cast<int>(files.size());
    // Per-file band-power envelope (time-domain, hop-aligned with the spectrogram).
    std::vector<std::vector<float>> env(nf, std::vector<float>(s.cols, 0.0f));
    std::vector<float> env_sum(s.cols, 0.0f);
    for (int f = 0; f < nf; ++f) {
        const auto& x = files[f].second;
        for (int c = 0; c < s.cols; ++c) {
            const int start = c * hop;
            double e = 0;
            for (int i = 0; i < fft_size && start + i < (int)x.size(); ++i)
                e += (double)x[start + i] * x[start + i];
            env[f][c] = (float)e;
            env_sum[c] += (float)e;
        }
    }
    // Noise floor = 10th-percentile env (the gaps); the channel is active >50% of the
    // time so the median is NOT noise. Threshold = geometric mean of floor and peak —
    // a robust midpoint between the noise and signal energy clusters.
    std::vector<float> sorted = env_sum;
    std::sort(sorted.begin(), sorted.end());
    const float floor_e = sorted[sorted.size() / 10] + 1e-12f;
    const float peak_e = sorted.back();
    const float thresh = std::sqrt(floor_e * peak_e);
    std::printf("  (energy floor=%.3g peak=%.3g thresh=%.3g)\n", floor_e, peak_e, thresh);

    // Band bins for bandwidth measure (200..3000 Hz).
    const int blo = std::max(1, (int)(200.0f / s.bin_hz));
    const int bhi = std::min(s.bins - 1, (int)(3000.0f / s.bin_hz));
    auto bw_at = [&](int c) {
        int act = 0;
        float pk = -1e30f;
        for (int b = blo; b <= bhi; ++b) pk = std::max(pk, s.db[(size_t)c * s.bins + b]);
        for (int b = blo; b <= bhi; ++b)
            if (s.db[(size_t)c * s.bins + b] > pk - 12.0f) ++act;
        return (float)act / (bhi - blo + 1);
    };

    // Active runs (merge gaps < 0.15s, drop runs < 0.08s).
    const int merge_gap = (int)(0.15f / s.col_sec);
    const int min_len = (int)(0.08f / s.col_sec);
    std::vector<std::pair<int, int>> runs;
    int c = 0;
    while (c < s.cols) {
        if (env_sum[c] > thresh) {
            int start = c;
            int gap = 0, last = c;
            while (c < s.cols && (env_sum[c] > thresh || gap < merge_gap)) {
                if (env_sum[c] > thresh) { last = c; gap = 0; } else { ++gap; }
                ++c;
            }
            if (last - start >= min_len) runs.push_back({start, last});
        } else ++c;
    }

    // Per-run mean bandwidth.
    std::vector<float> run_bw(runs.size());
    for (size_t ri = 0; ri < runs.size(); ++ri) {
        float bw = 0; int n = 0;
        for (int cc = runs[ri].first; cc <= runs[ri].second; cc += 4) { bw += bw_at(cc); ++n; }
        run_bw[ri] = bw / std::max(1, n);
    }
    // Setup ends at the first long + WIDEBAND run — OFDM data is ~0.72 full-band, while
    // the MC-DPSK CONNECT/CONNECT_ACK frames are narrower (~0.35), so duration alone is
    // not enough to tell a setup frame from a data group.
    int setup_end = s.cols;
    for (size_t ri = 0; ri < runs.size(); ++ri) {
        const float dur = (runs[ri].second - runs[ri].first) * s.col_sec;
        if (dur > 3.0f && run_bw[ri] > 0.55f) { setup_end = runs[ri].first; break; }
    }

    const int anchor_cols = (int)(1.41f / s.col_sec);  // descriptor full chirp+LTS
    std::vector<Segment> segs;
    for (size_t ri = 0; ri < runs.size(); ++ri) {
        auto& rn = runs[ri];
        const float dur = (rn.second - rn.first) * s.col_sec;
        const float bw = run_bw[ri];
        // Direction: file with most energy in this run is the RECEIVER; tx = the other.
        int dom = 0; float best = -1;
        for (int f = 0; f < nf; ++f) {
            float e = 0; for (int cc = rn.first; cc <= rn.second; ++cc) e += env[f][cc];
            if (e > best) { best = e; dom = f; }
        }
        auto stationOf = [&](int f) {
            std::string n2 = files[f].first;
            auto slash = n2.find_last_of("/\\"); if (slash != std::string::npos) n2 = n2.substr(slash + 1);
            auto us = n2.find('_'); if (us != std::string::npos) n2 = n2.substr(0, us);
            for (auto& ch : n2) ch = static_cast<char>(std::toupper((unsigned char)ch));
            return n2;
        };
        const std::string tx = (nf == 2) ? stationOf(1 - dom) : stationOf(dom);

        if (rn.first < setup_end) {
            // setup region
            Segment sg{rn.first, rn.second, Kind::Setup, "", tx, bw};
            sg.label = (dur < 2.0f) ? ("PING " + tx) : ("CONN " + tx);
            segs.push_back(sg);
        } else if (dur >= 3.0f && bw > 0.45f) {
            // a data group: split anchor (chirp+LTS) + OFDM data
            const int split = std::min(rn.second, rn.first + anchor_cols);
            segs.push_back({rn.first, split, Kind::Anchor, "ANCHOR", tx, bw});
            segs.push_back({split, rn.second, Kind::Data, "DATA " + tx, tx, bw});
        } else if (dur < 1.5f && bw < 0.4f) {
            segs.push_back({rn.first, rn.second, Kind::Ack, "ACK " + tx, tx, bw});
        } else {
            segs.push_back({rn.first, rn.second, Kind::Other, "?", tx, bw});
        }
    }
    return segs;
}

void printReport(const std::vector<Segment>& segs, const Spectrogram& s) {
    auto secs = [&](int c) { return c * s.col_sec; };
    int groups = 0, acks = 0;
    float anchor_t = 0, data_t = 0, ack_t = 0, setup_t = 0, active_t = 0;
    std::printf("\n================= AUTOMATED EXCHANGE ANALYSIS =================\n");
    std::printf("  #   t_start   dur    kind     dir        bw\n");
    int i = 0;
    for (const auto& g : segs) {
        const float dur = (g.c1 - g.c0) * s.col_sec;
        const char* kn = g.kind == Kind::Setup ? "setup" : g.kind == Kind::Anchor ? "anchor"
                        : g.kind == Kind::Data ? "DATA" : g.kind == Kind::Ack ? "ack" : "?";
        std::printf("  %-3d %7.2fs %6.2fs %-7s %-10s %4.0f%%\n", i++, secs(g.c0), dur, kn,
                    g.label.c_str(), g.bw * 100);
        active_t += dur;
        switch (g.kind) {
            case Kind::Setup: setup_t += dur; break;
            case Kind::Anchor: anchor_t += dur; break;
            case Kind::Data: data_t += dur; ++groups; break;
            case Kind::Ack: ack_t += dur; ++acks; break;
            default: break;
        }
    }
    const float total = s.cols * s.col_sec;
    const float gaps = total - active_t;
    std::printf("\n  ---- OVERHEAD SUMMARY (total %.1fs) ----\n", total);
    std::printf("  setup (MC-DPSK handshake) : %6.1fs  %4.1f%%\n", setup_t, 100 * setup_t / total);
    std::printf("  chirp+LTS anchors (%2d)    : %6.1fs  %4.1f%%   <- per-group re-acquire\n",
                groups, anchor_t, 100 * anchor_t / total);
    std::printf("  tone-burst ACKs (%2d)      : %6.1fs  %4.1f%%\n", acks, ack_t, 100 * ack_t / total);
    std::printf("  turnaround / gaps         : %6.1fs  %4.1f%%\n", gaps, 100 * gaps / total);
    std::printf("  OFDM DATA (payload)       : %6.1fs  %4.1f%%   <- the only payload-bearing time\n",
                data_t, 100 * data_t / total);
    std::printf("  ------------------------------------------------\n");
    std::printf("  overhead (everything but DATA): %.1f%%\n", 100 * (total - data_t) / total);
    std::printf("===============================================================\n\n");
}

int renderAnnotatedPng(const Spectrogram& s, const std::vector<Segment>& segs, float fmax,
                       float db_min, float db_max, int target_w, int freq_scale,
                       const std::string& out) {
    const int used_bins = std::min(s.bins, (int)(fmax / s.bin_hz) + 1);
    const int pool = std::max(1, (s.cols + target_w - 1) / target_w);
    const int WF = (s.cols + pool - 1) / pool;          // waterfall width
    const int WH = used_bins * freq_scale;              // waterfall height
    const int LANE = 22, TOP = LANE + 4, AXIS = 16, LEG = 18, LEFT = 4;
    const int W = WF + LEFT, H = TOP + WH + AXIS + LEG;
    std::vector<uint8_t> img(static_cast<size_t>(W) * H * 3, 18);

    // waterfall
    for (int px = 0; px < WF; ++px)
        for (int b = 0; b < used_bins; ++b) {
            float best = -200.0f;
            for (int k = 0; k < pool; ++k) {
                const int c = px * pool + k; if (c >= s.cols) break;
                best = std::max(best, s.db[(size_t)c * s.bins + b]);
            }
            uint8_t r, g, bl; colormap(dbNorm(best, db_min, db_max), r, g, bl);
            const int y = TOP + WH - 1 - b * freq_scale;
            for (int fs = 0; fs < freq_scale; ++fs) {
                uint8_t* p = &img[((size_t)(y - fs) * W + (LEFT + px)) * 3];
                p[0] = r; p[1] = g; p[2] = bl;
            }
        }
    auto initial = [](const std::string& s2) { return s2.empty() ? '?' : s2[0]; };
    // annotation lane: a colored bar per segment + short label
    for (const auto& sg : segs) {
        const int x0 = LEFT + sg.c0 / pool, x1 = LEFT + sg.c1 / pool;
        uint8_t r, g, b; kindColor(sg.kind, r, g, b);
        fillRect(img.data(), W, H, x0, 2, x1 - 1, 2 + LANE, r, g, b);
        fillRect(img.data(), W, H, x0, TOP, x0 + 1, TOP + WH, r, g, b, 0.35f);  // separator
        std::string lab;
        switch (sg.kind) {
            case Kind::Setup:  lab = sg.label.substr(0, sg.label.find(' ')); break;
            case Kind::Anchor: lab = "ANC"; break;
            case Kind::Data:   lab = std::string("DATA ") + initial(sg.tx); break;
            case Kind::Ack:    lab = std::string("ACK ") + initial(sg.tx); break;
            default:           lab = "?"; break;
        }
        const int sc = (x1 - x0 > (int)lab.size() * 12 + 4) ? 2
                     : (x1 - x0 > (int)lab.size() * 6 + 2) ? 1 : 0;
        if (sc) drawText(img.data(), W, H, x0 + 2, 4, sc, lab, 8, 8, 8);
    }
    // role-swap marker: first DATA whose transmitter differs from the first DATA's.
    std::string first_tx;
    for (const auto& sg : segs) if (sg.kind == Kind::Data) { first_tx = sg.tx; break; }
    for (const auto& sg : segs)
        if (sg.kind == Kind::Data && !sg.tx.empty() && sg.tx != first_tx) {
            const int x = LEFT + sg.c0 / pool;
            fillRect(img.data(), W, H, x - 1, TOP, x + 1, TOP + WH, 255, 255, 255, 0.8f);
            drawText(img.data(), W, H, x + 2, TOP + 2, 2, "ROLE-SWAP", 255, 255, 255);
            break;
        }
    // time axis ticks every 10s
    for (float t = 0; t < s.cols * s.col_sec; t += 10.0f) {
        const int x = LEFT + (int)(t / s.col_sec) / pool;
        fillRect(img.data(), W, H, x, TOP + WH, x + 1, TOP + WH + 4, 200, 200, 200);
        drawText(img.data(), W, H, x + 1, TOP + WH + 4, 1, std::to_string((int)t) + "S", 200, 200, 200);
    }
    // legend
    struct LE { Kind k; const char* n; };
    const LE legend[] = {{Kind::Setup, "MC-DPSK SETUP"}, {Kind::Anchor, "CHIRP+LTS ANCHOR"},
                         {Kind::Data, "OFDM DATA"}, {Kind::Ack, "TONE-BURST ACK"}};
    int lx = LEFT + 2, ly = TOP + WH + AXIS + 2;
    for (const auto& e : legend) {
        uint8_t r, g, b; kindColor(e.k, r, g, b);
        fillRect(img.data(), W, H, lx, ly, lx + 12, ly + 10, r, g, b);
        drawText(img.data(), W, H, lx + 16, ly + 2, 1, e.n, 220, 220, 220);
        lx += 16 + (int)std::strlen(e.n) * 6 + 24;
    }
    if (!stbi_write_png(out.c_str(), W, H, 3, img.data(), W * 3)) return 1;
    std::printf("wrote %s (%dx%d, %d segments)\n", out.c_str(), W, H, (int)segs.size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> wavs;
    std::string png_out, annotate_out;
    bool do_analyze = false;
    int fft_size = 1024, hop = 256, target_w = 2200, freq_scale = 3;
    float fmax = 4000.0f, db_min = 0, db_max = 0;
    float t0 = 0.0f, t1 = -1.0f;  // optional time crop (seconds)
    bool db_auto = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--png") png_out = next();
        else if (a == "--fft") fft_size = std::atoi(next());
        else if (a == "--hop") hop = std::atoi(next());
        else if (a == "--fmax") fmax = std::atof(next());
        else if (a == "--db-min") { db_min = std::atof(next()); db_auto = false; }
        else if (a == "--db-max") { db_max = std::atof(next()); db_auto = false; }
        else if (a == "--t0") t0 = std::atof(next());
        else if (a == "--t1") t1 = std::atof(next());
        else if (a == "--width") target_w = std::atoi(next());
        else if (a == "--freq-scale") freq_scale = std::atoi(next());
        else if (a == "--analyze") do_analyze = true;
        else if (a == "--annotate") { annotate_out = next(); do_analyze = true; }
        else if (a.size() && a[0] == '-') { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); return 2; }
        else wavs.push_back(a);
    }
    if (wavs.empty()) {
        std::fprintf(stderr,
            "usage: waterfall_viewer <wav...> [--png out.png | --annotate out.png | --analyze]\n"
            "         [--fft 1024] [--hop 256] [--fmax 4000] [--db-min N --db-max N]\n"
            "         [--t0 S --t1 S] [--width 2200]\n"
            "  --analyze / --annotate: pass the two _rx WAVs to segment + attribute direction.\n");
        return 2;
    }

    // Load each file; keep per-file samples (direction attribution) + build the sum.
    std::vector<std::pair<std::string, std::vector<float>>> loaded;
    std::vector<float> mix;
    for (const auto& path : wavs) {
        auto w = ultra::tools::io::loadWavMono48k(path);
        if (w.samples_48k.empty()) { std::fprintf(stderr, "empty/failed: %s\n", path.c_str()); return 1; }
        if (w.samples_48k.size() > mix.size()) mix.resize(w.samples_48k.size(), 0.0f);
        for (size_t i = 0; i < w.samples_48k.size(); ++i) mix[i] += w.samples_48k[i];
        std::printf("loaded %s (%.1fs)\n", path.c_str(), w.samples_48k.size() / 48000.0f);
        loaded.emplace_back(path, std::move(w.samples_48k));
    }

    // Optional time crop [t0, t1) — applied to the sum AND the per-file buffers so
    // analysis stays consistent (lets you render a readable annotated zoom of a region).
    if (t0 > 0.0f || t1 > 0.0f) {
        const size_t a = std::min(mix.size(), (size_t)(t0 * 48000.0f));
        const size_t b = (t1 > 0.0f) ? std::min(mix.size(), (size_t)(t1 * 48000.0f)) : mix.size();
        if (b > a) {
            mix = std::vector<float>(mix.begin() + a, mix.begin() + b);
            for (auto& f : loaded) {
                const size_t fb = std::min(f.second.size(), b);
                const size_t fa = std::min(f.second.size(), a);
                f.second = (fb > fa) ? std::vector<float>(f.second.begin() + fa, f.second.begin() + fb)
                                     : std::vector<float>();
            }
        }
        std::printf("cropped to %.2f-%.2fs (%.2fs)\n", t0, (t1 > 0 ? t1 : mix.size() / 48000.0f + t0),
                    mix.size() / 48000.0f);
    }

    Spectrogram s = computeSpectrogram(mix, fft_size, hop, 48000.0f);
    if (s.cols == 0) { std::fprintf(stderr, "audio too short for fft=%d\n", fft_size); return 1; }
    if (db_auto) {
        // Auto dynamic range: floor a bit above the noise, ceiling near peak.
        db_max = s.db_hi - 3.0f;
        db_min = s.db_hi - 70.0f;
    }
    std::printf("spectrogram: %d cols x %d bins, %.3fs/col, %.1f Hz/bin, dB range[%.0f..%.0f] (peak %.0f)\n",
                s.cols, s.bins, s.col_sec, s.bin_hz, db_min, db_max, s.db_hi);

    if (do_analyze) {
        auto segs = analyzeExchange(loaded, s, fft_size, hop);
        printReport(segs, s);
        if (!annotate_out.empty())
            return renderAnnotatedPng(s, segs, fmax, db_min, db_max, target_w, freq_scale, annotate_out);
        return 0;
    }

    if (!png_out.empty()) {
        return renderPng(s, fmax, db_min, db_max, target_w, freq_scale, png_out);
    }
#ifndef WATERFALL_NO_SDL
    return runInteractive(s, fmax, db_min, db_max);
#else
    std::fprintf(stderr, "built without SDL; use --png\n");
    return 2;
#endif
}

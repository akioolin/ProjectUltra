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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> wavs;
    std::string png_out;
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
        else if (a.size() && a[0] == '-') { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); return 2; }
        else wavs.push_back(a);
    }
    if (wavs.empty()) {
        std::fprintf(stderr,
            "usage: waterfall_viewer <wav...> [--png out.png] [--fft 1024] [--hop 256]\n"
            "                        [--fmax 4000] [--db-min N --db-max N] [--width 2200]\n");
        return 2;
    }

    // Load + sum (half-duplex → full timeline).
    std::vector<float> mix;
    for (const auto& path : wavs) {
        auto w = ultra::tools::io::loadWavMono48k(path);
        if (w.samples_48k.empty()) { std::fprintf(stderr, "empty/failed: %s\n", path.c_str()); return 1; }
        if (w.samples_48k.size() > mix.size()) mix.resize(w.samples_48k.size(), 0.0f);
        for (size_t i = 0; i < w.samples_48k.size(); ++i) mix[i] += w.samples_48k[i];
        std::printf("loaded %s (%.1fs)\n", path.c_str(), w.samples_48k.size() / 48000.0f);
    }

    // Optional time crop [t0, t1).
    if (t0 > 0.0f || t1 > 0.0f) {
        const size_t a = std::min(mix.size(), (size_t)(t0 * 48000.0f));
        const size_t b = (t1 > 0.0f) ? std::min(mix.size(), (size_t)(t1 * 48000.0f)) : mix.size();
        if (b > a) mix = std::vector<float>(mix.begin() + a, mix.begin() + b);
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

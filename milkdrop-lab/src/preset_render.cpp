// Headless MilkDrop preset renderer for the preset lab.
// Renders .milk presets through libprojectM with deterministic time + synthetic audio,
// writes PNG captures and a JSON metrics report.
#include <projectM-4/projectM.h>

#include <windows.h>
#include <GL/gl.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA8 0x8058

typedef void(APIENTRY* PFNGLGENFB)(GLsizei, GLuint*);
typedef void(APIENTRY* PFNGLBINDFB)(GLenum, GLuint);
typedef void(APIENTRY* PFNGLFBTEX)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY* PFNGLCHECKFB)(GLenum);
typedef HGLRC(WINAPI* PFNWGLCREATECTX)(HDC, HGLRC, const int*);

static PFNGLGENFB glGenFramebuffers_;
static PFNGLBINDFB glBindFramebuffer_;
static PFNGLFBTEX glFramebufferTexture2D_;
static PFNGLCHECKFB glCheckFramebufferStatus_;

// ---------------------------------------------------------------- options
struct Options
{
    std::vector<std::string> presets;
    std::string outDir = "out";
    std::string audio = "beat"; // silence | beat | quiet | wav:<file>
    std::string textures;       // texture search dir
    int width = 640, height = 480;
    int fps = 30;
    float duration = 12.0f;
    float gain = 1.0f;
    int meshW = 48, meshH = 36;
    std::vector<float> captures; // seconds
    bool verbose = false;
    bool strip = false;          // also dump a frame every 0.25s
};

// ---------------------------------------------------------------- logging
struct LogSink
{
    std::vector<std::string> lines;
};

static bool g_keepAll = false;

static void LogCb(const char* msg, projectm_log_level level, void* ud)
{
    // Always ask projectM for debug output (needs ENABLE_VERBOSE_LOGGING); keep warnings, errors and
    // shader-failure diagnostics, and everything with --verbose.
    const std::string m = msg;
    if (g_keepAll || level >= PROJECTM_LOG_LEVEL_WARN || m.find("rror") != std::string::npos || m.find("Failed") == 0 ||
        m.find("Failed") != std::string::npos)
    {
        if (!g_keepAll && m.size() > 4000)
        {
            static_cast<LogSink*>(ud)->lines.emplace_back(m.substr(0, 4000) + "...");
            return;
        }
        static_cast<LogSink*>(ud)->lines.emplace_back(m);
    }
}

struct FailSink
{
    bool failed = false;
    std::string message;
};

static void FailCb(const char*, const char* message, void* ud)
{
    auto* s = static_cast<FailSink*>(ud);
    s->failed = true;
    s->message = message ? message : "";
}

// ---------------------------------------------------------------- GL context
static bool CreateContext()
{
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "lab_gl";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("lab_gl", "lab", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC legacy = wglCreateContext(dc);
    wglMakeCurrent(dc, legacy);
    auto create = reinterpret_cast<PFNWGLCREATECTX>(wglGetProcAddress("wglCreateContextAttribsARB"));
    if (!create)
    {
        return false;
    }
    const int attribs[] = {0x2091, 3, 0x2092, 3, 0x9126, 0x1, 0}; // 3.3 core
    HGLRC ctx = create(dc, nullptr, attribs);
    if (!ctx)
    {
        return false;
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(legacy);
    wglMakeCurrent(dc, ctx);
    glGenFramebuffers_ = reinterpret_cast<PFNGLGENFB>(wglGetProcAddress("glGenFramebuffers"));
    glBindFramebuffer_ = reinterpret_cast<PFNGLBINDFB>(wglGetProcAddress("glBindFramebuffer"));
    glFramebufferTexture2D_ = reinterpret_cast<PFNGLFBTEX>(wglGetProcAddress("glFramebufferTexture2D"));
    glCheckFramebufferStatus_ = reinterpret_cast<PFNGLCHECKFB>(wglGetProcAddress("glCheckFramebufferStatus"));
    return glGenFramebuffers_ && glBindFramebuffer_ && glFramebufferTexture2D_ && glCheckFramebufferStatus_;
}

// ---------------------------------------------------------------- audio synth
struct AudioSource
{
    std::string mode;
    float gain = 1.0f;
    std::vector<float> wav; // interleaved stereo @44.1k
    size_t wavPos = 0;
    uint64_t n = 0;
    std::mt19937 rng{1234};

    static constexpr int kRate = 44100;

    bool LoadWav(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            return false;
        }
        std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0)
        {
            return false;
        }
        size_t p = 12;
        int ch = 2, bits = 16;
        while (p + 8 <= d.size())
        {
            uint32_t sz;
            std::memcpy(&sz, &d[p + 4], 4);
            if (!std::memcmp(&d[p], "fmt ", 4))
            {
                uint16_t c, b;
                std::memcpy(&c, &d[p + 10], 2);
                std::memcpy(&b, &d[p + 22], 2);
                ch = c;
                bits = b;
            }
            else if (!std::memcmp(&d[p], "data", 4))
            {
                if (bits != 16)
                {
                    return false;
                }
                const int16_t* s = reinterpret_cast<const int16_t*>(&d[p + 8]);
                size_t frames = std::min<size_t>(sz, d.size() - p - 8) / 2 / ch;
                for (size_t i = 0; i < frames; i++)
                {
                    float l = s[i * ch] / 32768.f, r = ch > 1 ? s[i * ch + 1] / 32768.f : l;
                    wav.push_back(l);
                    wav.push_back(r);
                }
                return !wav.empty();
            }
            p += 8 + sz + (sz & 1);
        }
        return false;
    }

    // Produce `frames` stereo sample pairs.
    void Generate(std::vector<float>& out, int frames)
    {
        out.resize(size_t(frames) * 2);
        std::uniform_real_distribution<float> noise(-1.f, 1.f);
        const double kPi = 3.14159265358979323846;
        for (int i = 0; i < frames; i++, n++)
        {
            float l = 0, r = 0;
            const double t = double(n) / kRate;
            if (mode == "silence")
            {
            }
            else if (!wav.empty())
            {
                l = wav[wavPos] * gain;
                r = wav[wavPos + 1] * gain;
                wavPos = (wavPos + 2) % wav.size();
            }
            else
            {
                // 120 bpm: kick on beats, hat on off-beats, snare-ish noise on 2 and 4, steady bass + pad.
                const double beat = t * 2.0;
                const double bp = std::fmod(beat, 1.0) / 2.0; // seconds into beat
                const double halfp = std::fmod(beat + 0.5, 1.0) / 2.0;
                const int bnum = int(beat) % 4;
                float s = 0;
                s += float(std::sin(2 * kPi * (45.0 * bp + 65.0 * (1 - std::exp(-25 * bp)) / 25.0)) * std::exp(-9 * bp)) * 0.9f;
                s += float(noise(rng) * std::exp(-60 * halfp)) * 0.25f;
                if (bnum == 1 || bnum == 3)
                {
                    s += float(noise(rng) * std::exp(-14 * bp)) * 0.35f;
                }
                s += float(std::sin(2 * kPi * 55.0 * t)) * 0.18f;
                s += float(std::sin(2 * kPi * 440.0 * t) + std::sin(2 * kPi * 554.4 * t) + std::sin(2 * kPi * 659.3 * t)) * 0.05f;
                l = r = s * gain * (mode == "quiet" ? 0.15f : 0.85f);
                r += noise(rng) * 0.01f * gain;
            }
            out[size_t(i) * 2] = std::clamp(l, -1.f, 1.f);
            out[size_t(i) * 2 + 1] = std::clamp(r, -1.f, 1.f);
        }
    }
};

// ---------------------------------------------------------------- metrics
struct Metrics
{
    double luma = 0, black = 0, clipped = 0, contrast = 0, sat = 0, diff = 0;
};

static Metrics Measure(const std::vector<uint8_t>& px, int w, int h, const std::vector<uint8_t>* prev)
{
    Metrics m;
    double sum = 0, sum2 = 0, sat = 0, diff = 0;
    size_t black = 0, clip = 0, n = size_t(w) * h;
    for (size_t i = 0; i < n; i++)
    {
        const float r = px[i * 4] / 255.f, g = px[i * 4 + 1] / 255.f, b = px[i * 4 + 2] / 255.f;
        const float l = 0.299f * r + 0.587f * g + 0.114f * b;
        sum += l;
        sum2 += double(l) * l;
        const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
        sat += mx > 0.001f ? (mx - mn) / mx : 0;
        black += l < 0.02f;
        clip += l > 0.95f;
        if (prev)
        {
            diff += (std::abs(int(px[i * 4]) - (*prev)[i * 4]) + std::abs(int(px[i * 4 + 1]) - (*prev)[i * 4 + 1]) +
                     std::abs(int(px[i * 4 + 2]) - (*prev)[i * 4 + 2])) / (3.0 * 255.0);
        }
    }
    m.luma = sum / n;
    m.contrast = std::sqrt(std::max(0.0, sum2 / n - m.luma * m.luma));
    m.black = double(black) / n;
    m.clipped = double(clip) / n;
    m.sat = sat / n;
    m.diff = prev ? diff / n : 0;
    return m;
}

static std::string JsonEscape(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '"' || c == '\\')
        {
            o += '\\';
            o += c;
        }
        else if (c == '\n')
        {
            o += "\\n";
        }
        else if (c == '\t')
        {
            o += "\\t";
        }
        else if (static_cast<unsigned char>(c) >= 0x20)
        {
            o += c;
        }
    }
    return o;
}

static std::string Stem(const std::string& path)
{
    std::string s = fs::path(path).stem().string();
    for (auto& c : s)
    {
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'))
        {
            c = '_';
        }
    }
    return s;
}

static void WriteLog(std::ostringstream& json, const LogSink& logs)
{
    json << "\"log\":[";
    for (size_t i = 0; i < logs.lines.size(); i++)
    {
        json << (i ? "," : "") << "\"" << JsonEscape(logs.lines[i]) << "\"";
    }
    json << "]";
}

// ---------------------------------------------------------------- main
int main(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--preset") o.presets.push_back(next());
        else if (a == "--out") o.outDir = next();
        else if (a == "--audio") o.audio = next();
        else if (a == "--textures") o.textures = next();
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &o.width, &o.height);
        else if (a == "--mesh") std::sscanf(next().c_str(), "%dx%d", &o.meshW, &o.meshH);
        else if (a == "--fps") o.fps = std::atoi(next().c_str());
        else if (a == "--duration") o.duration = float(std::atof(next().c_str()));
        else if (a == "--gain") o.gain = float(std::atof(next().c_str()));
        else if (a == "--strip") o.strip = true;
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--capture")
        {
            std::stringstream ss(next());
            std::string t;
            while (std::getline(ss, t, ',')) o.captures.push_back(float(std::atof(t.c_str())));
        }
    }
    if (o.presets.empty())
    {
        std::fprintf(stderr, "usage: preset_render --preset a.milk [--preset b.milk] --out DIR [--audio beat|quiet|silence|wav:f] "
                             "[--capture 1,4,8] [--duration 10] [--size 640x480] [--fps 30] [--gain 1] [--textures DIR] [--strip]\n");
        return 2;
    }
    if (o.captures.empty())
    {
        o.captures = {o.duration * 0.25f, o.duration * 0.6f, o.duration};
    }
    std::sort(o.captures.begin(), o.captures.end());

    if (!CreateContext())
    {
        std::fprintf(stderr, "failed to create OpenGL 3.3 core context\n");
        return 3;
    }

    LogSink logs;
    projectm_set_log_callback(LogCb, false, &logs);
    g_keepAll = o.verbose;
    projectm_set_log_level(PROJECTM_LOG_LEVEL_DEBUG, false);

    AudioSource audio;
    audio.mode = o.audio;
    audio.gain = o.gain;
    std::string audioLabel = o.audio;
    if (o.audio.rfind("wav:", 0) == 0)
    {
        if (!audio.LoadWav(o.audio.substr(4)))
        {
            std::fprintf(stderr, "cannot read 16-bit PCM wav: %s\n", o.audio.c_str());
            return 4;
        }
        audioLabel = "wav-" + Stem(o.audio.substr(4));
    }

    fs::create_directories(o.outDir);
    std::ostringstream json;
    json << "{\"audio\":\"" << JsonEscape(audioLabel) << "\",\"size\":[" << o.width << "," << o.height << "],\"fps\":" << o.fps
         << ",\"duration\":" << o.duration << ",\"presets\":[";

    bool firstPreset = true;
    int failures = 0;
    for (const auto& presetPath : o.presets)
    {
        logs.lines.clear();
        FailSink fail;
        const std::string stem = Stem(presetPath);
        const fs::path dir = fs::path(o.outDir) / stem / audioLabel;
        fs::create_directories(dir);

        if (!firstPreset) json << ",";
        firstPreset = false;
        json << "{\"file\":\"" << JsonEscape(presetPath) << "\",\"name\":\"" << JsonEscape(stem) << "\",";

        projectm_handle pm = projectm_create();
        if (!pm)
        {
            json << "\"loaded\":false,\"error\":\"projectm_create failed\",\"log\":[]}";
            failures++;
            continue;
        }
        projectm_set_window_size(pm, o.width, o.height);
        projectm_set_fps(pm, o.fps);
        projectm_set_mesh_size(pm, o.meshW, o.meshH);
        projectm_set_preset_locked(pm, true);
        projectm_set_hard_cut_enabled(pm, false);
        projectm_set_preset_switch_failed_event_callback(pm, FailCb, &fail);
        if (!o.textures.empty())
        {
            const char* paths[] = {o.textures.c_str()};
            projectm_set_texture_search_paths(pm, paths, 1);
        }

        GLuint tex = 0, fbo = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, o.width, o.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers_(1, &fbo);
        glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

        projectm_set_frame_time(pm, 0.0);
        projectm_load_preset_file(pm, presetPath.c_str(), false);

        if (fail.failed)
        {
            json << "\"loaded\":false,\"error\":\"" << JsonEscape(fail.message) << "\",";
            WriteLog(json, logs);
            json << "}";
            failures++;
            projectm_destroy(pm);
            glBindFramebuffer_(GL_FRAMEBUFFER, 0);
            glDeleteTextures(1, &tex);
            continue;
        }

        const int totalFrames = int(o.duration * o.fps) + 1;
        const int spf = AudioSource::kRate / o.fps;
        std::vector<float> pcm;
        std::vector<uint8_t> px(size_t(o.width) * o.height * 4), flipped(px.size()), prev;
        size_t capIdx = 0;
        json << "\"loaded\":true,\"captures\":[";
        bool firstCap = true;
        double lumaSum = 0, diffSum = 0;
        int lumaN = 0;
        std::vector<double> lumaTrace;
        for (int f = 0; f < totalFrames; f++)
        {
            const double t = double(f) / o.fps;
            audio.Generate(pcm, spf);
            projectm_pcm_add_float(pm, pcm.data(), spf, PROJECTM_STEREO);
            projectm_set_frame_time(pm, t);
            glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
            projectm_opengl_render_frame_fbo(pm, fbo);

            const bool wantCap = capIdx < o.captures.size() && t + 0.5 / o.fps >= o.captures[capIdx];
            const bool wantStrip = o.strip && (f % std::max(1, o.fps / 4) == 0);
            if (wantCap || wantStrip || (f % 3 == 0))
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, o.width, o.height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                for (int y = 0; y < o.height; y++)
                {
                    std::memcpy(&flipped[size_t(y) * o.width * 4], &px[size_t(o.height - 1 - y) * o.width * 4], size_t(o.width) * 4);
                }
                for (size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
                const Metrics m = Measure(flipped, o.width, o.height, prev.empty() ? nullptr : &prev);
                lumaSum += m.luma;
                diffSum += m.diff;
                lumaN++;
                lumaTrace.push_back(m.luma);
                prev = flipped;
                if (wantCap)
                {
                    char name[64];
                    std::snprintf(name, sizeof(name), "t%05.2f.png", t);
                    const fs::path png = dir / name;
                    stbi_write_png(png.string().c_str(), o.width, o.height, 4, flipped.data(), o.width * 4);
                    char buf[640];
                    std::snprintf(buf, sizeof(buf), "%s{\"t\":%.2f,\"png\":\"%s\",\"luma\":%.4f,\"black\":%.4f,\"clipped\":%.4f,\"contrast\":%.4f,\"sat\":%.4f,\"motion\":%.4f}",
                                  firstCap ? "" : ",", t, JsonEscape(png.generic_string()).c_str(), m.luma, m.black, m.clipped, m.contrast, m.sat, m.diff);
                    json << buf;
                    firstCap = false;
                    capIdx++;
                }
                else if (wantStrip)
                {
                    char name[64];
                    std::snprintf(name, sizeof(name), "strip_%05.2f.png", t);
                    stbi_write_png((dir / name).string().c_str(), o.width, o.height, 4, flipped.data(), o.width * 4);
                }
            }
        }
        double mean = lumaN ? lumaSum / lumaN : 0, var = 0;
        for (double v : lumaTrace) var += (v - mean) * (v - mean);
        var = lumaTrace.empty() ? 0 : var / lumaTrace.size();
        char sumBuf[256];
        std::snprintf(sumBuf, sizeof(sumBuf), "],\"mean_luma\":%.4f,\"luma_stddev\":%.4f,\"mean_motion\":%.4f,", mean, std::sqrt(var), lumaN ? diffSum / lumaN : 0);
        json << sumBuf;
        WriteLog(json, logs);
        json << "}";

        projectm_destroy(pm);
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &tex);
    }
    json << "]}\n";
    const fs::path reportPath = fs::path(o.outDir) / ("report-" + audioLabel + ".json");
    std::ofstream(reportPath) << json.str();
    std::printf("%s\n", reportPath.generic_string().c_str());
    return failures ? 1 : 0;
}

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"          

#include <mod/amlmod.h>
#include <mod/logger.h>
#include "imgui_internal.h" 

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <iomanip>

#include "main.h"
#include "arial.h"
#include "lyrics.h"
#include "fft_processor.h"

#include <map> // keyboard for special letters like enye

MYMOD(net.rusjj.imgui, DearImGui, 1.0.0, ocornut & RusJJ)

static IM imgui;
IImGui* pImGui   = &imgui;
ImGuiContext* imguiCtx = nullptr;

// ── Unicode ranges ───────────────────────
static const ImWchar ranges[] = {
    0x0020, 0x00FF, // Basic Latin + Latin-1 Supplement (Includes á, è, ñ, ß, €, etc.)
    0x0400, 0x0460, // Cyrillic base
    0x0490, 0x04A0, // Cyrillic extensions
    0x2010, 0x2040, // General Punctuation
    0x20A0, 0x20B0, // Currency Symbols
    0x2110, 0x2130, // Letterlike Symbols
    0 
};

// ── GTA SA hooks / globals ────────
uintptr_t  pGameLib    = 0;
void* pGameHandle = nullptr;
bool       bImGuiInitialized = false;

RwReal* nearScreenZ;
RwReal* recipNearClip;
void* pTheCamera = nullptr;
void  (*SetScissorRect)(float*);
int   (*GetScreenFadeStatus)(void*);
void  (*GTA_RequestKeyboard)(int);
bool* m_UserPause   = nullptr;

ImVec2 displaySize;
ImVec2 zeroVec(0,0);

static float flScaleX, flScaleY;
static int   nDisplayX, nDisplayY;
inline float ScaleX(float x) { return flScaleX * x; }
float IM::GetScaledX(float f) { return ScaleX(f); }
inline float ScaleY(float y) { return flScaleY * y; }
float IM::GetScaledY(float f) { return ScaleY(f); }
int   IM::GetScreenSizeX()    { return nDisplayX; }
int   IM::GetScreenSizeY()    { return nDisplayY; }

void ImGui_ImplRenderWare_RenderDrawData(ImDrawData*);
bool ImGui_ImplRenderWare_Init();
void ImGui_ImplRenderWare_NewFrame();
void ImGui_ImplRenderWare_ShutDown();

#define FRAMES_TO_CLEAR_MOUSE 3
static char nClearMousePos = 0;
ImFont* kbFont;

// ── Music path ───────────────────────────────────────────────
static constexpr const char* MUSIC_FOLDER =
     "/storage/emulated/0/Music";

static constexpr const char* CONFIG_PATH =
    "/storage/emulated/0/Android_unprotected/data/"
    "com.rockstargames.gtasa/configs/Musicplayer.ini";

// Supported extensions (miniaudio covers all of these)
static const char* SUPPORTED_EXT[] = {
    ".mp3",".MP3",
    ".ogg",".OGG",
    ".wav",".WAV",
    ".flac",".FLAC",
    ".opus",".OPUS",
    ".aac",".AAC",
    ".m4a",".M4A",
    ".wma",".WMA",
    ".aiff",".AIFF",
    ".au",".AU",
    ".raw",".RAW",
    nullptr
};

// ── Music Player State ───────────────────────────────────────
struct MusicTrack {
    std::string filename;   // display name (without path)
    std::string fullpath;
};

static std::vector<MusicTrack> g_tracks;
static int    g_currentTrack  = -1;   // index into g_tracks
static bool   g_isPlaying     = false;
static bool   g_bLoop         = false;
static bool   g_bShuffle      = false;

// miniaudio engine + sound
static ma_engine g_engine;
static ma_sound  g_sound;
static bool      g_engineReady = false;
static bool      g_soundLoaded = false;

// Visualizer bar data  (32 bars)
#define VIS_BARS 32
static float g_visBars[VIS_BARS]   = {};
static float g_visTarget[VIS_BARS] = {};
static float g_visPhase = 0.0f;

// ── REAL AUDIO TRACKING METRICS ──
static float g_audioFrequencies[VIS_BARS] = {};

// ── Snow fall effect ──────────────────────────────────────────
struct SnowFlake {
    float xFrac, yFrac;   // position as fraction (0..1) of the window's size
    float speed;          // fall speed (fraction of window height per second)
    float radius;         // flake size in pixels
    float drift;          // horizontal sway amplitude (fraction per second)
    float driftPhase;     // sway phase accumulator
};
#define SNOW_FLAKE_COUNT 140
static std::vector<SnowFlake> g_snowFlakes;
static bool g_bSnowInitialized = false;

// Library touch dragging state mechanics
static float g_dragScrollY = 0.0f;

// Helper variables for the vertical quick-scroll bar
static float g_customScrollPercent = 0.0f;
static bool  g_isDraggingSlider = false;

// Auto-scroll: when a new track starts, animate list to centre it
static int   g_scrollToTrack = -1;
static float g_scrollTarget  = 0.0f;

// Shuffle history for proper PREV support
static std::vector<int> g_shuffleHistory;
static int g_shuffleHistoryPos = -1;

// ── Helper: extension check ──────────────────────────────────
static bool IsAudioFile(const char* name)
{
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    for (int i = 0; SUPPORTED_EXT[i]; i++)
        if (strcasecmp(dot, SUPPORTED_EXT[i]) == 0) return true;
    return false;
}

// ── Scan music folder ────────────────────────────────────────
static void ScanMusicFolder()
{
    g_tracks.clear();
    DIR* dir = opendir(MUSIC_FOLDER);
    if (!dir) {
        logger->Error("Music folder not accessible: %s", MUSIC_FOLDER);
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) continue;
        if (!IsAudioFile(entry->d_name)) continue;
        MusicTrack t;
        t.filename = entry->d_name;
        t.fullpath = std::string(MUSIC_FOLDER) + "/" + entry->d_name;
        g_tracks.push_back(t);
    }
    closedir(dir);
    // Sort alphabetically
    std::sort(g_tracks.begin(), g_tracks.end(),
        [](const MusicTrack& a, const MusicTrack& b){
            return a.filename < b.filename;
        });
    logger->Info("Music player: found %zu tracks", g_tracks.size());
}

// ── Audio helpers ────────────────────────────────────────────
static void StopAndUnload()
{
    if (g_soundLoaded) {
        ma_sound_stop(&g_sound);
        ma_sound_uninit(&g_sound);
        g_soundLoaded = false;
    }
    g_isPlaying = false;
}

static bool LoadAndPlay(int idx)
{
    if (idx < 0 || idx >= (int)g_tracks.size()) return false;
    if (!g_engineReady) return false;

    StopAndUnload();

    ma_result res = ma_sound_init_from_file(
        &g_engine,
        g_tracks[idx].fullpath.c_str(),
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC,
        nullptr, nullptr, &g_sound);

    if (res != MA_SUCCESS) {
        logger->Error("Failed to load: %s", g_tracks[idx].fullpath.c_str());
        return false;
    }

    ma_sound_set_looping(&g_sound, g_bLoop ? MA_TRUE : MA_FALSE);
    ma_sound_start(&g_sound);
    g_soundLoaded = true;
    g_isPlaying   = true;
    g_currentTrack = idx;
    g_scrollToTrack = idx;  // trigger auto-scroll to this track
    
    // Reset accuracy logs on start
    for (int i = 0; i < VIS_BARS; i++) {
        g_audioFrequencies[i] = 0.0f;
    }
    
    // ── LYRIC LOAD HOOK ──
    LoadLyricsForSong(g_tracks[idx].fullpath);
    
    // Keep shuffle history in sync when a track is loaded directly
    if (g_shuffleHistoryPos < 0 ||
        (g_shuffleHistoryPos < (int)g_shuffleHistory.size() && g_shuffleHistory[g_shuffleHistoryPos] != idx)) {
        // Truncate forward history and append
        if (g_shuffleHistoryPos + 1 < (int)g_shuffleHistory.size())
            g_shuffleHistory.erase(g_shuffleHistory.begin() + g_shuffleHistoryPos + 1, g_shuffleHistory.end());
        g_shuffleHistory.push_back(idx);
        g_shuffleHistoryPos = (int)g_shuffleHistory.size() - 1;
    }
    logger->Info("Now playing: %s", g_tracks[idx].filename.c_str());
    return true;
}

std::vector<int> g_filteredIdx;

static void PlayNext()
{
    if (g_tracks.empty()) return;
    int next;
    if (g_bShuffle) {
        // If we're not at the end of history, move forward in it
        if (g_shuffleHistoryPos < (int)g_shuffleHistory.size() - 1) {
            g_shuffleHistoryPos++;
            next = g_shuffleHistory[g_shuffleHistoryPos];
        } else {
            {
                int poolSize = g_filteredIdx.empty() ? (int)g_tracks.size() : (int)g_filteredIdx.size();
                if (poolSize <= 1) {
                    next = g_filteredIdx.empty() ? 0 : g_filteredIdx[0];
                } else {
                    int attempts = 0;
                    do {
                        int idx = rand() % poolSize;
                        next = g_filteredIdx.empty() ? idx : g_filteredIdx[idx];
                        attempts++;
                    } while (next == g_currentTrack && attempts < 20);
                }
            }
                                        
            g_shuffleHistoryPos++;
            g_shuffleHistory.push_back(next);
            // Cap history size to avoid unbounded growth
            if ((int)g_shuffleHistory.size() > 200) {
                g_shuffleHistory.erase(g_shuffleHistory.begin());
                g_shuffleHistoryPos--;
            }
        }
    } else {
        if (!g_filteredIdx.empty()) {
            auto it = std::find(g_filteredIdx.begin(), g_filteredIdx.end(), g_currentTrack);
            int pos = (it != g_filteredIdx.end()) ? (int)(it - g_filteredIdx.begin()) : -1;
            next = g_filteredIdx[(pos + 1) % (int)g_filteredIdx.size()];
        } else {
            next = (g_currentTrack + 1) % (int)g_tracks.size();
        }
        
    }
    LoadAndPlay(next);
}

static void PlayPrev()
{
    if (g_tracks.empty()) return;
    int prev;
    if (g_bShuffle) {
        // Go back in shuffle history if possible
        if (g_shuffleHistoryPos > 0) {
            g_shuffleHistoryPos--;
            prev = g_shuffleHistory[g_shuffleHistoryPos];
        } else {
            // Already at the start of history, stay on current
            prev = (g_currentTrack >= 0) ? g_currentTrack : 0;
        }
    } else {
        if (!g_filteredIdx.empty()) {
            auto it = std::find(g_filteredIdx.begin(), g_filteredIdx.end(), g_currentTrack);
            int pos = (it != g_filteredIdx.end()) ? (int)(it - g_filteredIdx.begin()) : 0;
            prev = g_filteredIdx[(pos - 1 + (int)g_filteredIdx.size()) % (int)g_filteredIdx.size()];
        } else {
            prev = (g_currentTrack - 1 + (int)g_tracks.size()) % (int)g_tracks.size();
        }
    }
    LoadAndPlay(prev);
}

static void TogglePlayPause()
{
    if (!g_engineReady) return;
    if (!g_soundLoaded) {
        // Nothing loaded yet — play first track
        if (!g_tracks.empty()) LoadAndPlay(0);
        return;
    }
    if (g_isPlaying) {
        ma_sound_stop(&g_sound);
        g_isPlaying = false;
    } else {
        ma_sound_start(&g_sound);
        g_isPlaying = true;
    }
}

static void StopPlayback()
{
    StopAndUnload();
    g_currentTrack = -1;
    // Reset visualizer
    for (int i = 0; i < VIS_BARS; i++) g_visBars[i] = 0.0f;
}

// ── Playback progress ────────────────────────────────────────
static float GetProgress()
{
    if (!g_soundLoaded) return 0.0f;
    ma_uint64 cur = 0, len = 0;
    ma_sound_get_cursor_in_pcm_frames(&g_sound, &cur);
    ma_sound_get_length_in_seconds(&g_sound, nullptr); // legacy matching
    ma_sound_get_length_in_pcm_frames(&g_sound, &len);
    if (len == 0) return 0.0f;
    return (float)cur / (float)len;
}

static float GetDurationSec()
{
    if (!g_soundLoaded) return 0.0f;
    float sec = 0.0f;
    ma_sound_get_length_in_seconds(&g_sound, &sec);
    return sec;
}

static float GetPositionSec()
{
    if (!g_soundLoaded) return 0.0f;
    float sec = 0.0f;
    ma_sound_get_cursor_in_seconds(&g_sound, &sec);
    return sec;
}

static void SeekToProgress(float p)
{
    if (!g_soundLoaded) return;
    ma_uint64 len = 0;
    ma_sound_get_length_in_pcm_frames(&g_sound, &len);
    ma_uint64 target = (ma_uint64)(p * (float)len);
    ma_sound_seek_to_pcm_frame(&g_sound, target);
}

static void FormatTime(char* buf, size_t sz, float sec)
{
    int m = (int)sec / 60;
    int s = (int)sec % 60;
    snprintf(buf, sz, "%d:%02d", m, s);
}
//

//
// ── Visualizer update ────────────────────────────────────────
// ── Per-bar peak hold state (for Digital Blocks) ─────────
static float g_visPeak[VIS_BARS]      = {};  // peak dot position
static float g_visPeakTimer[VIS_BARS] = {};  // hold timer before falling
static int   g_visualizerType         = 0;

static void UpdateVisualizer(float dt)
{
    if (dt > 0.1f) dt = 0.1f;

    // Fluid drop-off to zero if the music player stops
    if (!g_isPlaying || !g_soundLoaded || &g_sound.engineNode == nullptr) {
        for (int i = 0; i < VIS_BARS; i++) {
            g_visBars[i] -= dt * 2.5f;
            if (g_visBars[i] < 0.0f) g_visBars[i] = 0.0f;
            g_visPeak[i] -= dt * 1.8f;
            if (g_visPeak[i] < g_visBars[i]) g_visPeak[i] = g_visBars[i];
        }
        return;
    }

    g_visPhase += dt;

    // ── STEP A: EXTRACT RAW LIVE SAMPLES FROM MINIAUDIO ──
    std::complex<float> fftBuffer[FFT_SIZE] = { 0.0f };
    
    ma_uint64 readCursor = 0;
    ma_sound_get_cursor_in_pcm_frames(&g_sound, &readCursor);
    
    if (g_sound.pDataSource != nullptr) {
        float rawBuffer[FFT_SIZE * 2]; // Interleaved stereo channel allocation
        ma_uint64 framesRead = 0;
        
        ma_uint64 currentPosition = 0;
        ma_data_source_get_cursor_in_pcm_frames(g_sound.pDataSource, &currentPosition);
        ma_data_source_read_pcm_frames(g_sound.pDataSource, rawBuffer, FFT_SIZE, &framesRead);
        ma_data_source_seek_to_pcm_frame(g_sound.pDataSource, currentPosition); // Maintain playback sync
        
        if (framesRead > 10) {
            for (ma_uint64 i = 0; i < framesRead && i < FFT_SIZE; i++) {
                float left  = rawBuffer[i * 2];
                float right = rawBuffer[i * 2 + 1];
                
                float multiplier = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (FFT_SIZE - 1)));
                fftBuffer[i] = std::complex<float>((left + right) * 0.5f * multiplier, 0.0f);
            }
        }
    }

    // ── STEP B: PROCESS REALS THROUGH TRANSFORM ──
    ComputeFFT(fftBuffer, FFT_SIZE); 

    // ── STEP C: LOGARITHMIC FREQUENCY BINNING (WITH AUTOMATIC CEILING SCALING) ──
    float volumeModifier = ma_sound_get_volume(&g_sound);
    int halfBins = FFT_SIZE / 2;
    
    // Dynamic tracking variable to calculate structural headroom peaks
    static float s_adaptiveCeiling = 1.0f;
    float frameMaxTarget = 0.0f;

    for (int i = 0; i < VIS_BARS; i++) {
        float norm = (float)i / (float)VIS_BARS;
        int startBin = (int)(powf(halfBins, norm));
        int endBin   = (int)(powf(halfBins, (float)(i + 1) / (float)VIS_BARS));
        if (endBin <= startBin) endBin = startBin + 1;
        if (endBin > halfBins)  endBin = halfBins;

        float magnitudeSum = 0.0f;
        int count = 0;
        for (int b = startBin; b < endBin; b++) {
            magnitudeSum += std::abs(fftBuffer[b]);
            count++;
        }
        float averageMag = (count > 0) ? (magnitudeSum / count) : 0.0f;

        // Raw logarithmic decibel tracking 
        float dbScaled = 20.0f * log10f(1.0f + averageMag * 6.0f);
        float rawTarget = dbScaled * volumeModifier;

        if (rawTarget > frameMaxTarget) {
            frameMaxTarget = rawTarget;
        }

        g_visTarget[i] = rawTarget;
    }

    // AGC Normalization Window adjustment
    if (frameMaxTarget > s_adaptiveCeiling) {
        s_adaptiveCeiling = frameMaxTarget; // Instantly scale up to capture massive drops
    } else {
        s_adaptiveCeiling += (frameMaxTarget - s_adaptiveCeiling) * (dt * 0.5f); // Smooth decay
    }
    
    if (s_adaptiveCeiling < 0.2f) s_adaptiveCeiling = 0.2f;

    // Apply scaling factor across all bins
    for (int i = 0; i < VIS_BARS; i++) {
        // Multiplied by 0.88f to cleanly pad the top edge, leaving responsive headroom space
        g_visTarget[i] = (g_visTarget[i] / s_adaptiveCeiling) * 0.88f;

        if (g_visTarget[i] < 0.01f) g_visTarget[i] = 0.0f;
    }

    // ── STEP D: RENDER PHYSICS INTERPOLATION BY TYPE ──
    if (g_visualizerType == 0 || g_visualizerType == 1)
    {
        // Smooth waveform / line connection blending
        for (int i = 0; i < VIS_BARS; i++) {
            float diff = g_visTarget[i] - g_visBars[i];
            g_visBars[i] += diff * (dt * 13.5f); 
            if (g_visBars[i] < 0.0f) g_visBars[i] = 0.0f;
            if (g_visBars[i] > 1.0f) g_visBars[i] = 1.0f;
        }
    }
    else
    {
        // Snappy digital blocks / independent spectrum bars
        for (int i = 0; i < VIS_BARS; i++) {
            float diff = g_visTarget[i] - g_visBars[i];
            
            if (diff > 0.0f) {
                g_visBars[i] += diff * (dt * 24.0f); // Fast response upward
            } else {
                g_visBars[i] += diff * (dt * 12.5f); // Fast decay response downward
            }

            if (g_visBars[i] < 0.0f) g_visBars[i] = 0.0f;
            if (g_visBars[i] > 1.0f) g_visBars[i] = 1.0f;

            // Peak Dot Logic
            if (g_visBars[i] >= g_visPeak[i]) {
                g_visPeak[i]      = g_visBars[i];
                g_visPeakTimer[i] = 0.45f;
            } else {
                g_visPeakTimer[i] -= dt;
                if (g_visPeakTimer[i] < 0.0f) {
                    g_visPeak[i] -= dt * 1.1f;
                }
                if (g_visPeak[i] < g_visBars[i]) g_visPeak[i] = g_visBars[i];
                if (g_visPeak[i] < 0.0f)         g_visPeak[i] = 0.0f;
            }
        }
    }
}

static void CheckTrackEnd()
{
    if (!g_soundLoaded || !g_isPlaying) return;
    if (ma_sound_at_end(&g_sound)) {
        if (g_bLoop) {
            ma_sound_seek_to_pcm_frame(&g_sound, 0);
            ma_sound_start(&g_sound);
        } else {
            PlayNext();
        }
    }
}

// MusicButton dynamically pulls colors directly from the currently active ImGui style theme
static bool MusicButton(const char* label, ImVec2 size, bool active = false)
{
    ImGuiStyle& style = ImGui::GetStyle();

    // ── EXTRACTION FORCE ──
    // Extracts the precise transparency setting currently assigned to your theme windows
    float touchAlpha = style.Colors[ImGuiCol_WindowBg].w;

    if (active) {
        // Construct colors using CheckMark's RGB, but preserve transparency rules!
        ImVec4 activeCol = ImVec4(style.Colors[ImGuiCol_CheckMark].x, style.Colors[ImGuiCol_CheckMark].y, style.Colors[ImGuiCol_CheckMark].z, touchAlpha);
        ImGui::PushStyleColor(ImGuiCol_Button,        activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  activeCol);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        style.Colors[ImGuiCol_Button]);
        
        // ── TARGETED TOUCH ALPHA FIX ──
        // Overrides the solid 1.0f checkmark alpha channel during interaction 
        // with your active theme alpha value!
        ImVec4 actionCol = ImVec4(style.Colors[ImGuiCol_CheckMark].x, style.Colors[ImGuiCol_CheckMark].y, style.Colors[ImGuiCol_CheckMark].z, touchAlpha);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, actionCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  actionCol);
    }
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

// ── Snow fall effect (rendered inside the Music Player window) ──
static void InitSnow()
{
    g_snowFlakes.clear();
    g_snowFlakes.reserve(SNOW_FLAKE_COUNT);
    for (int i = 0; i < SNOW_FLAKE_COUNT; i++) {
        SnowFlake f;
        f.xFrac      = (float)(rand() % 1000) / 1000.0f;        
        f.yFrac      = (float)(rand() % 1000) / 1000.0f;        
        f.speed      = 0.06f  + (float)(rand() % 100) / 1000.0f; 
        f.radius     = 1.2f   + (float)(rand() % 25) / 10.0f;    
        f.drift      = 0.01f  + (float)(rand() % 30) / 1000.0f;  
        f.driftPhase = (float)(rand() % 628) / 100.0f;           
        g_snowFlakes.push_back(f);
    }
    g_bSnowInitialized = true;
}

static void DrawSnowEffect(float dt, ImVec2 winPos, ImVec2 winSize, ImDrawList* dl)
{
    if (!g_bSnowInitialized) InitSnow();
    if (winSize.x <= 1.0f || winSize.y <= 1.0f) return;

    dl->PushClipRect(winPos, ImVec2(winPos.x + winSize.x, winPos.y + winSize.y), true);

    for (auto& f : g_snowFlakes) {
        f.yFrac      += f.speed * dt;
        f.driftPhase += dt;
        f.xFrac      += sinf(f.driftPhase) * f.drift * dt;

        if (f.yFrac > 1.0f) {
            f.yFrac = -0.02f;
            f.xFrac = (float)(rand() % 1000) / 1000.0f;
        }
        if (f.xFrac > 1.0f) f.xFrac -= 1.0f;
        if (f.xFrac < 0.0f) f.xFrac += 1.0f;

        ImVec2 pos(winPos.x + f.xFrac * winSize.x, winPos.y + f.yFrac * winSize.y);

        int alpha = 140 + (int)(f.radius * 25.0f);
        if (alpha > 255) alpha = 255;
        ImU32 col = IM_COL32(255, 255, 255, alpha);
        dl->AddCircleFilled(pos, ScaleX(f.radius), col, 8);
    }

    dl->PopClipRect();
}

// ── Menu visibility flag ─────────────────────────────────────
bool bDisplaySpecialImGuiMenu = false;
bool g_bLockWindow = false;
bool g_bLockOpenCloseBtn = false;
bool g_bLockKeyboard = false; // Add this globally near the top of main.cpp
bool g_bBtnNoBg      = false;
bool g_bSnowEffect = false;
bool g_bPendingThemeApply = false; // set by LoadConfig, consumed on first frame
float g_fDragBgScale = 14.0f;
ImVec4 g_customButtonOpenCloseCol = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // open/close button
ImGuiID LastFocus = -1, LastActive = -1;
ImGuiWindow* LastWindow;

// ── Promoted settings globals (were static locals in DrawMusicPlayer) ────
int    g_theme           = 0;
ImVec4 g_customTextCol      = ImVec4(0.92f, 0.86f, 0.88f, 1.0f);
ImVec4 g_customThemeCol     = ImVec4(0.56f, 0.28f, 0.33f, 1.0f);
ImVec4 g_customHighlightCol = ImVec4(0.78f, 0.42f, 0.49f, 1.0f);

// ── INI config state ─────────────────────────────────────────
struct PlayerConfig {
    // [Window]
    float winPosX  = -1.0f;   // -1 = use layout default
    float winPosY  = -1.0f;
    float winSizeX = -1.0f;
    float winSizeY = -1.0f;
    // [ToggleButton]
    float btnPosX  = -1.0f;
    float btnPosY  = -1.0f;
    float btnScale = 14.0f;
    // [Visualizer]
    int   visualizerType = 0;
    // [Theme]
    int   currentTheme   = 0;
    // [CustomTheme]  stored as 0-255 per channel
    ImVec4 customText      = ImVec4(0.92f, 0.86f, 0.88f, 1.0f);
    ImVec4 customTheme     = ImVec4(0.56f, 0.28f, 0.33f, 1.0f);
    ImVec4 customHighlight = ImVec4(0.78f, 0.42f, 0.49f, 1.0f);
    ImVec4 customButton    = ImVec4(1.0f,  1.0f,  1.0f,  1.0f);
    // [Effects]
    bool snowFall   = false;
        float kbPosX  = -1.0f;
    float kbPosY  = -1.0f;
    float kbSizeX = -1.0f;
    float kbSizeY = -1.0f;
    // [Lock]
    bool lockWindow   = false;
    bool lockButton   = false;
    bool lockKeyboard = false;
    bool btnNoBg       = false;
} g_config;

// ── INI helpers ──────────────────────────────────────────────
static void TrimStr(std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static ImVec4 ParseColor(const std::string& val)
{
    int r = 255, g = 0, b = 255, a = 255;
    sscanf(val.c_str(), "%d %d %d %d", &r, &g, &b, &a);
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

static std::string ColorToStr(const ImVec4& c)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%d %d %d %d",
        (int)(c.x * 255.0f + 0.5f),
        (int)(c.y * 255.0f + 0.5f),
        (int)(c.z * 255.0f + 0.5f),
        (int)(c.w * 255.0f + 0.5f));
    return buf;
}

static void LoadConfig()
{
    g_bPendingThemeApply = true; // always apply theme on startup, even if no config file
    std::ifstream f(CONFIG_PATH);
    if (!f.is_open()) return;

    std::string line, section;
    while (std::getline(f, line)) {
        size_t cmt = line.find("//");
        if (cmt != std::string::npos) line = line.substr(0, cmt);
        cmt = line.find(';');
        if (cmt != std::string::npos) line = line.substr(0, cmt);
        TrimStr(line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos)
                section = line.substr(1, end - 1);
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        TrimStr(key); TrimStr(val);
        if (key.empty() || val.empty()) continue;

        if (section == "Window") {
            if      (key == "PosX")  g_config.winPosX  = (float)atof(val.c_str()) * displaySize.x;
            else if (key == "PosY")  g_config.winPosY  = (float)atof(val.c_str()) * displaySize.y;
            else if (key == "SizeX") g_config.winSizeX = (float)atof(val.c_str()) * displaySize.x;
            else if (key == "SizeY") g_config.winSizeY = (float)atof(val.c_str()) * displaySize.y;
        } else if (section == "ToggleButton") {
            if      (key == "PosX")  g_config.btnPosX = (float)atof(val.c_str()) * displaySize.x;
            else if (key == "PosY")  g_config.btnPosY = (float)atof(val.c_str()) * displaySize.y;
            else if (key == "Scale") g_config.btnScale = (float)atof(val.c_str());
            else if (key == "NoBg")  g_config.btnNoBg  = (atoi(val.c_str()) != 0);
        } else if (section == "Visualizer") {
            if (key == "Type") g_config.visualizerType = atoi(val.c_str());
        } else if (section == "Theme") {
            if (key == "CurrentTheme") g_config.currentTheme = atoi(val.c_str());
        } else if (section == "CustomTheme") {
            if      (key == "Text")      g_config.customText      = ParseColor(val);
            else if (key == "Theme")     g_config.customTheme     = ParseColor(val);
            else if (key == "Highlight") g_config.customHighlight = ParseColor(val);
            else if (key == "button")    g_config.customButton    = ParseColor(val);
        } else if (section == "Effects") {
            if (key == "SnowFall") g_config.snowFall = (atoi(val.c_str()) != 0);
        } else if (section == "Keyboard") {
            if      (key == "PosX")  g_config.kbPosX  = (float)atof(val.c_str()) * displaySize.x;
            else if (key == "PosY")  g_config.kbPosY  = (float)atof(val.c_str()) * displaySize.y;
            else if (key == "SizeX") g_config.kbSizeX = (float)atof(val.c_str()) * displaySize.x;
            else if (key == "SizeY") g_config.kbSizeY = (float)atof(val.c_str()) * displaySize.y;
        } else if (section == "Lock") {
            if      (key == "LockWindow")   g_config.lockWindow   = (atoi(val.c_str()) != 0);
            else if (key == "LockButton")   g_config.lockButton   = (atoi(val.c_str()) != 0);
            else if (key == "LockKeyboard") g_config.lockKeyboard = (atoi(val.c_str()) != 0);
        }
    }

    // Apply to runtime globals
    g_visualizerType           = g_config.visualizerType;
    g_theme                    = g_config.currentTheme;
    g_customTextCol            = g_config.customText;
    g_customThemeCol           = g_config.customTheme;
    g_customHighlightCol       = g_config.customHighlight;
    g_customButtonOpenCloseCol = g_config.customButton;
    g_bSnowEffect              = g_config.snowFall;
    g_bLockWindow              = g_config.lockWindow;
    g_bLockOpenCloseBtn        = g_config.lockButton;
    g_bLockKeyboard            = g_config.lockKeyboard;
    g_fDragBgScale             = g_config.btnScale;
    g_bBtnNoBg                 = g_config.btnNoBg;
    // Theme is applied to ImGui style once the context is ready (deferred to first frame).
    // Set flag so DrawMusicPlayer applies it on the first render.
    g_bPendingThemeApply = true;
}

static void SaveConfig()
{
    // Snapshot runtime state
    g_config.visualizerType  = g_visualizerType;
    g_config.currentTheme    = g_theme;
    g_config.customText      = g_customTextCol;
    g_config.customTheme     = g_customThemeCol;
    g_config.customHighlight = g_customHighlightCol;
    g_config.customButton    = g_customButtonOpenCloseCol;
    g_config.snowFall        = g_bSnowEffect;
    g_config.lockWindow      = g_bLockWindow;
    g_config.lockButton      = g_bLockOpenCloseBtn;
    g_config.lockKeyboard    = g_bLockKeyboard;
    g_config.btnScale        = g_fDragBgScale;
    g_config.btnNoBg         = g_bBtnNoBg;
    // winPosX/Y, winSizeX/Y, btnPosX/Y are updated every frame by TrackWindowPositions()

    std::ofstream f(CONFIG_PATH);
    if (!f.is_open()) {
        logger->Error("MusicPlayer: failed to open config for writing: %s", CONFIG_PATH);
        return;
    }

    f << std::fixed << std::setprecision(4);

    f << "[Window]\n";
    f << "PosX  = " << (displaySize.x > 0 ? g_config.winPosX  / displaySize.x : 0.0f) << "\n";
    f << "PosY  = " << (displaySize.y > 0 ? g_config.winPosY  / displaySize.y : 0.0f) << "\n";
    f << "SizeX = " << (displaySize.x > 0 ? g_config.winSizeX / displaySize.x : 0.0f) << "\n";
    f << "SizeY = " << (displaySize.y > 0 ? g_config.winSizeY / displaySize.y : 0.0f) << "\n\n";

    f << "[ToggleButton]\n";
    f << "PosX  = " << (displaySize.x > 0 ? g_config.btnPosX / displaySize.x : 0.0f) << "\n";
    f << "PosY  = " << (displaySize.y > 0 ? g_config.btnPosY / displaySize.y : 0.0f) << "\n";
    f << std::defaultfloat;
    f << "Scale = " << (int)g_config.btnScale << "\n\n";
    f << std::fixed << std::setprecision(4);
    f << "NoBg  = " << (g_config.btnNoBg ? 1 : 0) << " ; no background\n\n";

    f << "[Visualizer]\n";
    f << "Type = " << g_config.visualizerType << "\n";
    f << "; 0 = Fluid Waveform\n";
    f << "; 1 = Stereo Spectrum\n";
    f << "; 2 = Digital Blocks\n";
    f << "; 3 = Line Bar\n";
    f << "; 4 = Line\n";
    f << "; 5 = Lyrics\n";
    f << "; 6 = None\n\n";

    f << "[Theme]\n";
    f << "CurrentTheme = " << g_config.currentTheme << "\n";
    f << "; 0 = Default\n";
    f << "; 1 = Dark Gray\n";
    f << "; 2 = Old Rose\n";
    f << "; 3 = Teal\n";
    f << "; 4 = Purple\n";
    f << "; 5 = Chili Red\n";
    f << "; 6 = Custom\n\n";

    f << "[CustomTheme]\n";
    f << "Text      = " << ColorToStr(g_config.customText)      << "\n";
    f << "Theme     = " << ColorToStr(g_config.customTheme)     << "\n";
    f << "Highlight = " << ColorToStr(g_config.customHighlight) << "\n";
    f << "button    = " << ColorToStr(g_config.customButton)    << " ; open/close button\n\n";

    f << "[Effects]\n";
    f << "SnowFall = " << (g_config.snowFall ? 1 : 0) << "\n\n";

    f << "[Keyboard]\n";
    f << "PosX  = " << (displaySize.x > 0 ? g_config.kbPosX  / displaySize.x : 0.0f) << "\n";
    f << "PosY  = " << (displaySize.y > 0 ? g_config.kbPosY  / displaySize.y : 0.0f) << "\n";
    f << "SizeX = " << (displaySize.x > 0 ? g_config.kbSizeX / displaySize.x : 0.0f) << "\n";
    f << "SizeY = " << (displaySize.y > 0 ? g_config.kbSizeY / displaySize.y : 0.0f) << "\n\n";

    f << "[Lock]\n";
    f << "LockWindow   = " << (g_config.lockWindow   ? 1 : 0) << " ; no move/no resize\n";
    f << "LockButton   = " << (g_config.lockButton   ? 1 : 0) << " ; open/close button no move only\n";
    f << "LockKeyboard = " << (g_config.lockKeyboard ? 1 : 0) << " ; no move/no resize\n";

    logger->Info("MusicPlayer: config saved to %s", CONFIG_PATH);
}

// Called every frame while ImGui is live to cache window positions into g_config
// so SaveConfig() can write them without needing an active ImGui context.
static void TrackWindowPositions()
{
    if (ImGuiWindow* w = ImGui::FindWindowByName("Music Player")) {
        if (w->Active) {
            g_config.winPosX  = w->Pos.x;
            g_config.winPosY  = w->Pos.y;
            g_config.winSizeX = w->Size.x;
            g_config.winSizeY = w->Size.y;
        }
    }
    if (ImGuiWindow* w = ImGui::FindWindowByName("MusicPlayerToggle")) {
        g_config.btnPosX = w->Pos.x;
        g_config.btnPosY = w->Pos.y;
    }
    if (ImGuiWindow* w = ImGui::FindWindowByName("Keyboard")) {
        if (w->Active) {
            g_config.kbPosX  = w->Pos.x;
            g_config.kbPosY  = w->Pos.y;
            g_config.kbSizeX = w->Size.x;
            g_config.kbSizeY = w->Size.y;
        }
    }
}

// ── Apply a preset or custom theme to ImGui style ────────────
// Called both when the user picks from the dropdown AND on startup
// after LoadConfig restores g_theme and g_custom*Col globals.
static void ApplyThemeToStyle(int theme)
{
    if (theme < 0) return;
    ImGuiStyle& s = ImGui::GetStyle();

    if (theme == 0) { // Default Blue
        s.Colors[ImGuiCol_Text]             = ImVec4(0.92f, 0.95f, 1.00f, 1.0f);
        s.Colors[ImGuiCol_TextDisabled]     = ImVec4(0.55f, 0.62f, 0.75f, 1.0f);
        if (g_config.winPosX < 0.0f) { // no config loaded — set defaults
        g_customTextCol      = s.Colors[ImGuiCol_Text];
        g_customThemeCol     = ImVec4(0.20f, 0.40f, 0.70f, 1.0f);
        g_customHighlightCol = ImVec4(0.35f, 0.65f, 1.00f, 1.0f);
        }
        s.Colors[ImGuiCol_WindowBg]         = ImVec4(0.05f, 0.07f, 0.15f, 0.97f);
        s.Colors[ImGuiCol_TitleBgActive]    = ImVec4(0.08f, 0.14f, 0.32f, 1.0f);
        s.Colors[ImGuiCol_TitleBg]          = ImVec4(0.08f, 0.14f, 0.32f, 1.0f);
        s.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(s.Colors[ImGuiCol_TitleBg].x, s.Colors[ImGuiCol_TitleBg].y, s.Colors[ImGuiCol_TitleBg].z, 0.5f);
        s.Colors[ImGuiCol_Border]           = ImVec4(s.Colors[ImGuiCol_TitleBg].x, s.Colors[ImGuiCol_TitleBg].y, s.Colors[ImGuiCol_TitleBg].z, 1.0f);
        s.Colors[ImGuiCol_Button]           = ImVec4(0.12f, 0.30f, 0.60f, 1.0f);
        s.Colors[ImGuiCol_ButtonHovered]    = ImVec4(0.18f, 0.40f, 0.75f, 1.0f);
        s.Colors[ImGuiCol_ButtonActive]     = ImVec4(0.10f, 0.25f, 0.50f, 1.0f);
        s.Colors[ImGuiCol_SliderGrab]       = ImVec4(0.25f, 0.55f, 0.90f, 1.0f);
        s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.35f, 0.65f, 1.00f, 1.0f);
        s.Colors[ImGuiCol_FrameBg]          = ImVec4(0.08f, 0.14f, 0.30f, 1.0f);
        s.Colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.12f, 0.20f, 0.40f, 1.0f);
        s.Colors[ImGuiCol_FrameBgActive]    = ImVec4(0.08f, 0.14f, 0.30f, 1.0f);
        s.Colors[ImGuiCol_CheckMark]        = ImVec4(0.25f, 0.55f, 0.90f, 1.0f);
        s.Colors[ImGuiCol_Header]           = ImVec4(0.12f, 0.30f, 0.60f, 1.0f);
        s.Colors[ImGuiCol_HeaderHovered]    = ImVec4(0.18f, 0.40f, 0.75f, 1.0f);
        s.Colors[ImGuiCol_ScrollbarGrab]    = ImVec4(0.20f, 0.40f, 0.70f, 1.0f);
        s.Colors[ImGuiCol_ScrollbarGrabHovered] = s.Colors[ImGuiCol_ScrollbarGrab];
        s.Colors[ImGuiCol_ScrollbarGrabActive]  = s.Colors[ImGuiCol_ScrollbarGrab];
        s.Colors[ImGuiCol_ScrollbarBg]      = ImVec4(0.04f, 0.07f, 0.18f, 1.0f);
        s.Colors[ImGuiCol_Separator]        = ImVec4(0.15f, 0.30f, 0.55f, 0.8f);
        s.Colors[ImGuiCol_ResizeGrip]       = ImVec4(0.12f, 0.30f, 0.60f, 0.6f);
        s.Colors[ImGuiCol_ResizeGripHovered]= ImVec4(0.18f, 0.40f, 0.75f, 0.9f);
        s.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.10f, 0.25f, 0.50f, 1.0f);
    }
    else if (theme == 1) { // Dark Gray
        s.Colors[ImGuiCol_Text]             = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
        s.Colors[ImGuiCol_TextDisabled]     = ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
        g_customTextCol      = s.Colors[ImGuiCol_Text];
        g_customThemeCol     = ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
        g_customHighlightCol = ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    }
    else if (theme == 2) { // Old Rose
        s.Colors[ImGuiCol_Text]             = ImVec4(0.92f, 0.86f, 0.88f, 1.0f);
        s.Colors[ImGuiCol_TextDisabled]     = ImVec4(0.60f, 0.50f, 0.52f, 1.0f);
        g_customTextCol      = s.Colors[ImGuiCol_Text];
        g_customThemeCol     = ImVec4(0.56f, 0.28f, 0.33f, 1.0f);
        g_customHighlightCol = ImVec4(0.78f, 0.42f, 0.49f, 1.0f);
    }
    else if (theme == 3) { // Teal
        s.Colors[ImGuiCol_Text]             = ImVec4(0.90f, 0.98f, 0.98f, 1.0f);
        s.Colors[ImGuiCol_TextDisabled]     = ImVec4(0.50f, 0.70f, 0.72f, 1.0f);
        g_customTextCol      = s.Colors[ImGuiCol_Text];
        g_customThemeCol     = ImVec4(0.06f, 0.40f, 0.46f, 1.0f);
        g_customHighlightCol = ImVec4(0.15f, 0.75f, 0.85f, 1.0f);
    }
    else if (theme == 4) { // Purple
        s.Colors[ImGuiCol_Text]             = ImVec4(0.95f, 0.90f, 1.00f, 1.0f);
        s.Colors[ImGuiCol_TextDisabled]     = ImVec4(0.60f, 0.52f, 0.72f, 1.0f);
        g_customTextCol      = s.Colors[ImGuiCol_Text];
        g_customThemeCol     = ImVec4(0.42f, 0.18f, 0.72f, 1.0f);
        g_customHighlightCol = ImVec4(0.70f, 0.45f, 1.00f, 1.0f);
    }
    else if (theme == 5) { // Chili Red
        s.Colors[ImGuiCol_Text]             = ImVec4(1.00f, 0.94f, 0.92f, 1.0f);
        s.Colors[ImGuiCol_TextDisabled]     = ImVec4(0.75f, 0.55f, 0.55f, 1.0f);
        g_customTextCol      = s.Colors[ImGuiCol_Text];
        g_customThemeCol     = ImVec4(0.76f, 0.14f, 0.14f, 1.0f);
        g_customHighlightCol = ImVec4(1.00f, 0.35f, 0.35f, 1.0f);
    }

    // Themes 1-5: apply the custom theme/highlight colors using the shared custom logic.
    // Theme 6 (Custom) and Theme 0 (Default Blue) already set all colors above.
    if (theme >= 1 && theme <= 5) {
        // Reuse the Custom theme apply path with the preset colors we just set
        theme = 6;
    }
    //
    if (theme == 6) { // Custom (also used as final-pass for presets 1-5)
        float a = g_customThemeCol.w;

        // Clamp to minimum brightness so black doesn't make everything invisible
        float minBright = 0.17f; // 0.08
        float tr = std::max(g_customThemeCol.x, minBright);
        float tg = std::max(g_customThemeCol.y, minBright);
        float tb = std::max(g_customThemeCol.z, minBright);

        float lum2 = 0.299f*tr + 0.587f*tg + 0.114f*tb;
        ImVec4 baseCol, touchCol;
        if (lum2 > 0.6f) {
            baseCol  = ImVec4(tr * 0.6f, tg * 0.6f, tb * 0.6f, a);
            touchCol = ImVec4(tr, tg, tb, a);
        } else {
            baseCol  = ImVec4(tr, tg, tb, a);
            touchCol = ImVec4(
                std::min(tr * 1.4f, 1.0f),
                std::min(tg * 1.4f, 1.0f),
                std::min(tb * 1.4f, 1.0f), a);
        }
        float bgMin = 0.02f;
        s.Colors[ImGuiCol_Text]             = g_customTextCol;
        s.Colors[ImGuiCol_TextDisabled]     = ImVec4(g_customTextCol.x*0.60f, g_customTextCol.y*0.60f, g_customTextCol.z*0.60f, g_customTextCol.w);
        s.Colors[ImGuiCol_TitleBgActive]    = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
        s.Colors[ImGuiCol_TitleBg]          = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
        s.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a * 0.5f);
        s.Colors[ImGuiCol_Border]           = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), 0.80f);
        s.Colors[ImGuiCol_CheckMark]        = ImVec4(touchCol.x, touchCol.y, touchCol.z, g_customTextCol.w);
        s.Colors[ImGuiCol_SliderGrab]           = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
        s.Colors[ImGuiCol_SliderGrabActive]     = g_customHighlightCol;
        s.Colors[ImGuiCol_ScrollbarGrab]        = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
        s.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
        s.Colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
        s.Colors[ImGuiCol_ScrollbarBg]      = ImVec4(std::max(tr*0.30f, bgMin*0.5f), std::max(tg*0.30f, bgMin*0.5f), std::max(tb*0.30f, bgMin*0.5f), a);
        s.Colors[ImGuiCol_WindowBg]         = ImVec4(std::max(tr*0.80f, bgMin), std::max(tg*0.82f, bgMin), std::max(tb*0.82f, bgMin), a*0.95f);
        s.Colors[ImGuiCol_FrameBg]          = ImVec4(std::max(tr*0.60f, bgMin), std::max(tg*0.60f, bgMin), std::max(tb*0.60f, bgMin), a);
        s.Colors[ImGuiCol_FrameBgHovered]   = ImVec4(std::max(tr*0.75f, bgMin), std::max(tg*0.75f, bgMin), std::max(tb*0.75f, bgMin), a);
        s.Colors[ImGuiCol_FrameBgActive]    = ImVec4(std::max(tr*0.60f, bgMin), std::max(tg*0.60f, bgMin), std::max(tb*0.60f, bgMin), a);
        s.Colors[ImGuiCol_Button]           = baseCol;
        s.Colors[ImGuiCol_ButtonHovered]    = touchCol;
        s.Colors[ImGuiCol_ButtonActive]     = touchCol;
        s.Colors[ImGuiCol_Header]           = baseCol;
        s.Colors[ImGuiCol_HeaderHovered]    = touchCol;
        float mixFactor = (lum2 < 0.3f) ? 0.40f : 0.15f;
        s.Colors[ImGuiCol_Separator] = ImVec4(
            std::max(tr*(1.0f-mixFactor)+(1.0f*mixFactor), bgMin),
            std::max(tg*(1.0f-mixFactor)+(1.0f*mixFactor), bgMin),
            std::max(tb*(1.0f-mixFactor)+(1.0f*mixFactor), bgMin), 0.80f);
        s.Colors[ImGuiCol_ResizeGrip]       = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a*0.6f);
        s.Colors[ImGuiCol_ResizeGripHovered]= touchCol;
        s.Colors[ImGuiCol_ResizeGripActive] = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
    }
}
        // Redesigned Loop Icon matching your image
static void DrawLoopIcon(ImDrawList* drawList, ImVec2 minPos, ImVec2 maxPos, ImU32 color, bool isActive)
{
    float width = maxPos.x - minPos.x;
    float height = maxPos.y - minPos.y;
    ImVec2 center = ImVec2(minPos.x + width * 0.5f, minPos.y + height * 0.5f);

    float scale = ImGui::GetIO().FontGlobalScale;
    float sizeX = 24.0f * scale;  // 18.0
    float sizeY = 16.0f * scale;  // 12.0
    float arrowSize = 6.0f * scale; // 4.0
    float thickness = 4.0f; // 3.0

    // Outer box corners
    ImVec2 p1 = ImVec2(center.x - sizeX, center.y - sizeY); // Top-Left
    ImVec2 p2 = ImVec2(center.x + sizeX, center.y - sizeY); // Top-Right
    ImVec2 p3 = ImVec2(center.x + sizeX, center.y + sizeY); // Bottom-Right
    ImVec2 p4 = ImVec2(center.x - sizeX, center.y + sizeY); // Bottom-Left

    // Draw the loop box (Top, Right, Left lines)
    drawList->AddLine(p1, p2, color, thickness);
    drawList->AddLine(p2, p3, color, thickness);
    drawList->AddLine(p4, p1, color, thickness);

    // Bottom path with arrow gap
    float gapOffset = 6.0f * scale; // 4.0
    drawList->AddLine(p4, ImVec2(center.x - gapOffset, p4.y), color, thickness);
    drawList->AddLine(ImVec2(center.x + gapOffset, p3.y), p3, color, thickness);

    // The arrow head pointing right on the bottom track
    drawList->AddTriangleFilled(
        ImVec2(center.x - gapOffset, p4.y - arrowSize),
        ImVec2(center.x - gapOffset, p4.y + arrowSize),
        ImVec2(center.x - gapOffset + (arrowSize * 1.5f), p4.y),
        color
    );

    // Only draw the "1" if the loop is active
    if (isActive) {
        char oneStr[] = "1";
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]); // Ensure consistent font
        ImVec2 ts = ImGui::CalcTextSize(oneStr);
        drawList->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), color, oneStr);
        ImGui::PopFont();
    }
}

// Redesigned Shuffle Icon matching your image
static void DrawShuffleIcon(ImDrawList* drawList, ImVec2 minPos, ImVec2 maxPos, ImU32 color, bool isActive)
{
    float width = maxPos.x - minPos.x;
    float height = maxPos.y - minPos.y;
    ImVec2 center = ImVec2(minPos.x + width * 0.5f, minPos.y + height * 0.5f);

    float scale = ImGui::GetIO().FontGlobalScale;
    float sizeX = 26.0f * scale;  // 18.0
    float sizeY = 16.0f * scale;  // 12.0
    float arrowSize = 6.0f * scale; // 4.0
    float thickness = 4.0f; // 3.0

    if (isActive) center.y -= 4.0f * scale;

    float leftX   = center.x - sizeX;
    float rightX  = center.x + sizeX;
    float midLeftX  = center.x - (sizeX * 0.4f);
    float midRightX = center.x + (sizeX * 0.4f);
    
    float topY    = center.y - sizeY;
    float bottomY = center.y + sizeY;

    // Line 1: Top-Left -> Straight -> Cross down -> Straight to Bottom-Right
    drawList->AddLine(ImVec2(leftX, topY), ImVec2(midLeftX, topY), color, thickness);
    drawList->AddBezierCubic(ImVec2(midLeftX, topY), ImVec2(center.x, topY), ImVec2(center.x, bottomY), ImVec2(midRightX, bottomY), color, thickness);
    drawList->AddLine(ImVec2(midRightX, bottomY), ImVec2(rightX, bottomY), color, thickness);
    // Arrow head on bottom right line
    drawList->AddTriangleFilled(ImVec2(rightX - (arrowSize * 1.2f), bottomY - arrowSize), ImVec2(rightX - (arrowSize * 1.2f), bottomY + arrowSize), ImVec2(rightX, bottomY), color);

    // Line 2: Bottom-Left -> Straight -> Cross up -> Straight to Top-Right
    drawList->AddLine(ImVec2(leftX, bottomY), ImVec2(midLeftX, bottomY), color, thickness);
    drawList->AddBezierCubic(ImVec2(midLeftX, bottomY), ImVec2(center.x, bottomY), ImVec2(center.x, topY), ImVec2(midRightX, topY), color, thickness);
    drawList->AddLine(ImVec2(midRightX, topY), ImVec2(rightX, topY), color, thickness);
    // Arrow head on top right line
    drawList->AddTriangleFilled(ImVec2(rightX - (arrowSize * 1.2f), topY - arrowSize), ImVec2(rightX - (arrowSize * 1.2f), topY + arrowSize), ImVec2(rightX, topY), color);

    // Clean status indicator dot placed underneath the icon
    if (isActive) {
        ImVec2 dotPos = ImVec2(center.x, center.y + sizeY + (6.0f * scale));
        drawList->AddCircleFilled(dotPos, 2.5f * scale, color);
    }
}

static bool g_bShowKeyboard = false;
static bool g_bShiftActive  = false;

static void DrawImGuiKeyboard(char* buf, size_t bufSize)
{
    ImGuiStyle& style = ImGui::GetStyle();
    
    // --- Layout Definitions ---
    static const char* row1[] = {"1","2","3","4","5","6","7","8","9","0"};
    static const char* row2[] = {"q","w","e","r","t","y","u","i","o","p"};
    static const char* row3[] = {"a","s","d","f","g","h","j","k","l","-"};
    static const char* row4[] = {"z","x","c","v","b","n","m",",",".","/"};

    static const char* sym1[] = {"1","2","3","4","5","6","7","8","9","0"};
    static const char* sym2[] = {"[","]","{","}","#","%","^","*","+","="};
    static const char* sym3[] = {"_","\\","|","~","<",">","!","@","$","&"};
    static const char* sym4[] = {"?", "&&", ";", ":", "(", ")", "\"", "'", "`", "€"};

    // --- Special Variant Map for Long Press ---
    static std::map<char, std::vector<std::string>> specialVariants = {
        {'a', {"á", "à", "â", "ä", "ã", "å"}},
        {'e', {"é", "è", "ê", "ë"}},
        {'i', {"í", "ì", "î", "ï"}},
        {'o', {"ó", "ò", "ô", "ö", "õ", "ø"}},
        {'u', {"ú", "ù", "û", "ü"}},
        {'n', {"ñ"}},
        {'c', {"ç"}},
        {'s', {"ß"}}
    };

    // Keyboard State Tracking
    static bool g_bSymbolModeActive = false;
    static float holdTime = 0.0f;
    static char activeLongPressKey = 0;
    static bool openVariantPopup = false;
    static bool popupJustOpened = false; // Prevents click-release interference

    // Custom Design Variables
    float horizontalSpacing = ScaleX(9.0f); // Increase this to push buttons further apart horizontally (Original: 6.0f)
    float verticalSpacing   = ScaleY(5.0f); // Increase this to push rows further apart vertically (Original: 8.0f)
    float buttonRounding    = 10.0f;         // Higher number = more bent/circular corners (Original: 6.0f)
    float kbWidthDefault    = ScaleX(1680.0f);

    ImVec2 ds = ImGui::GetIO().DisplaySize;
    static bool s_kbPosSet = false;
    if (!s_kbPosSet) {
        ImVec2 kbSize = (g_config.kbSizeX > 0.0f && g_config.kbSizeY > 0.0f)
            ? ImVec2(g_config.kbSizeX, g_config.kbSizeY)
            : ImVec2(kbWidthDefault, ScaleY(450.0f));
        ImVec2 kbPos = (g_config.kbPosX >= 0.0f && g_config.kbPosY >= 0.0f)
            ? ImVec2(g_config.kbPosX, g_config.kbPosY)
            : ImVec2(ds.x * 0.5f, ds.y * 0.75f);
        ImGui::SetNextWindowSize(kbSize, ImGuiCond_Always);
        ImGui::SetNextWindowPos(kbPos, ImGuiCond_Always,
            (g_config.kbPosX >= 0.0f) ? ImVec2(0.0f, 0.0f) : ImVec2(0.5f, 0.5f));
        s_kbPosSet = true;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(ScaleX(400.0f), ScaleY(200.0f)), ImVec2(ds.x, ds.y)); // ScaleY 200.0
    // Contrasting title text color based on theme luminance
    float lum = 0.299f * g_customThemeCol.x + 0.587f * g_customThemeCol.y + 0.114f * g_customThemeCol.z;

    // Use gray for title text to match keyboard box color
    ImVec4 titleTextCol = (lum > 0.5f) 
        ? ImVec4(0.40f, 0.40f, 0.40f, 1.0f)  // dark gray on light theme
        : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);    // white on dark theme
    
    ImGui::PushStyleColor(ImGuiCol_Text, titleTextCol);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(horizontalSpacing, verticalSpacing));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, buttonRounding);

    ImGuiWindowFlags kbFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
    if (g_bLockKeyboard) kbFlags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (ImGui::Begin("Keyboard", &g_bShowKeyboard, kbFlags))
    {
        // Key buttons always use white text
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImVec2 contentSize = ImGui::GetContentRegionAvail();
        float kbWidth = contentSize.x;
        float kbHeight = contentSize.y;

        float keyW = (kbWidth - horizontalSpacing * 9.0f) / 10.0f;
        float totalGapsHeight = verticalSpacing * 4.0f;
        float keyH = (kbHeight - totalGapsHeight) / 5.0f;

        if (keyH < 10.0f) keyH = 10.0f;

        auto DrawRow = [&](const char** keys, int count) {
            for (int i = 0; i < count; i++) {
                if (i > 0) ImGui::SameLine(0, horizontalSpacing);
                
                std::string finalLabel = keys[i];
                char baseChar = finalLabel[0];
                
                if (g_bShiftActive && !g_bSymbolModeActive && finalLabel.length() == 1 && isalpha(baseChar)) {
                    finalLabel[0] = (char)toupper(baseChar);
                }

                ImGui::PushID(i + (int)(size_t)keys);
                
                // If a sub-popup is open, we disable active clicks on the underlying keyboard buttons
                bool pressed = false;
                if (!openVariantPopup) {
                    pressed = ImGui::Button(finalLabel.c_str(), ImVec2(keyW, keyH));
                } else {
                    ImGui::Button(finalLabel.c_str(), ImVec2(keyW, keyH)); // Visual only, non-clickable
                }
                
                // Track long-press holding duration
                if (ImGui::IsItemActive() && !g_bSymbolModeActive && specialVariants.count(baseChar) > 0) {
                    holdTime += ImGui::GetIO().DeltaTime;
                    if (holdTime > 0.35f && !openVariantPopup) { 
                        activeLongPressKey = baseChar;
                        openVariantPopup = true;
                        popupJustOpened = true; // Mark that popup took control
                    }
                }
                
                if (ImGui::IsItemDeactivated()) {
                    // Check if this was a quick click rather than a long press hold
                    if (holdTime <= 0.35f && pressed && !popupJustOpened) {
                        size_t len = strlen(buf);
                        if (len + finalLabel.length() < bufSize) { 
                            snprintf(buf + len, bufSize - len, "%s", finalLabel.c_str());
                        }
                    }
                    holdTime = 0.0f; 
                }                

                ImGui::PopID();
            }
        };

        if (g_bSymbolModeActive) {
            DrawRow(sym1, 10); DrawRow(sym2, 10); DrawRow(sym3, 10); DrawRow(sym4, 10);
        } else {
            DrawRow(row1, 10); DrawRow(row2, 10); DrawRow(row3, 10); DrawRow(row4, 10);
        }

        // --- Bottom Utility Row Layout ---
        float commandW = (kbWidth - horizontalSpacing * 3.0f) / 4.0f;

        if (g_bSymbolModeActive) {
            ImGui::BeginDisabled(); ImGui::Button("Shift", ImVec2(commandW, keyH)); ImVec2 dummy = ImGui::GetItemRectMin(); ImGui::EndDisabled();
        } else {
            if (ImGui::Button(g_bShiftActive ? "Shift*" : "Shift", ImVec2(commandW, keyH))) g_bShiftActive = !g_bShiftActive;
        }
        ImGui::SameLine(0, horizontalSpacing);

        if (ImGui::Button(g_bSymbolModeActive ? "Abc" : "123", ImVec2(commandW, keyH))) g_bSymbolModeActive = !g_bSymbolModeActive;
        ImGui::SameLine(0, horizontalSpacing);
        
        if (ImGui::Button("Space", ImVec2(commandW, keyH))) {
            size_t len = strlen(buf);
            if (len + 1 < bufSize) { buf[len] = ' '; buf[len+1] = '\0'; }
        }
        ImGui::SameLine(0, horizontalSpacing);
        
        if (ImGui::Button("Backspace", ImVec2(commandW, keyH))) {
            size_t len = strlen(buf);
            if (len > 0) {
                // Safely delete multi-byte UTF-8 characters instead of leaving broken bytes
                while (len > 0) {
                    len--;
                    if ((buf[len] & 0xC0) != 0x80) { 
                        buf[len] = '\0';
                        break;
                    }
                }
            }
        }
        
        // --- Dynamic Long-Press Context Menu Overlay ---
        if (openVariantPopup && popupJustOpened) {
            ImGui::OpenPopup("SpecialCharsPopup");
            popupJustOpened = false; // Reset setup trigger
        }

        ImGui::PushStyleColor(ImGuiCol_PopupBg,        style.Colors[ImGuiCol_WindowBg]);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, buttonRounding);

        if (ImGui::BeginPopup("SpecialCharsPopup")) 
        {
            auto& variants = specialVariants[activeLongPressKey];
            for (size_t v = 0; v < variants.size(); v++) {
                if (v > 0) ImGui::SameLine(0, horizontalSpacing);
                
                std::string itemLabel = variants[v];
                if (g_bShiftActive) {
                    static const std::map<std::string, std::string> upperMap = {
                        {"á","Á"},{"à","À"},{"â","Â"},{"ä","Ä"},{"ã","Ã"},{"å","Å"},
                        {"é","É"},{"è","È"},{"ê","Ê"},{"ë","Ë"},
                        {"í","Í"},{"ì","Ì"},{"î","Î"},{"ï","Ï"},
                        {"ó","Ó"},{"ò","Ò"},{"ô","Ô"},{"ö","Ö"},{"õ","Õ"},{"ø","Ø"},
                        {"ú","Ú"},{"ù","Ù"},{"û","Û"},{"ü","Ü"},
                        {"ñ","Ñ"},{"ç","Ç"},{"ß","ß"}
                    };
                    auto it = upperMap.find(itemLabel);
                    if (it != upperMap.end()) itemLabel = it->second;
                }
                
                        
                // Sub-button configuration inside the panel overlay
                if (ImGui::Button(itemLabel.c_str(), ImVec2(keyW, keyH))) {
                    size_t len = strlen(buf);
                    if (len + itemLabel.length() < bufSize) {
                        snprintf(buf + len, bufSize - len, "%s", itemLabel.c_str());
                    }
                
                    openVariantPopup = false;
                    ImGui::CloseCurrentPopup();
                }
            }

            // Close the selection window if the user taps elsewhere
            if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered()) {
                openVariantPopup = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        else {
            // Force reset state if ImGui closed the popup automatically
            openVariantPopup = false;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}
//
static void DrawMusicPlayer(bool* p_open)
{
    static float lastTime = 0.0f;
    float now = (float)ImGui::GetTime();
    float dt  = now - lastTime;
    if (dt > 0.1f) dt = 0.1f;
    lastTime  = now;

    UpdateVisualizer(dt);
    CheckTrackEnd();
    
    // ── LYRIC TIME TRACKING HOOK ──
    if (g_isPlaying && g_soundLoaded) {
        UpdateLyricsSync(GetPositionSec()); // Directly queries miniaudio's current cursor timestamp
    }

    // Fetch the actual current alpha configured by your slider theme
    float currentThemeAlpha = ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w;
    ImGui::SetNextWindowBgAlpha(currentThemeAlpha); 
    
    if (g_config.winSizeX > 0.0f && g_config.winSizeY > 0.0f) {
        ImGui::SetNextWindowSize(
            ImVec2(g_config.winSizeX, g_config.winSizeY),
            ImGuiCond_Once);
    } else {
        ImGui::SetNextWindowSize(
            ImVec2(displaySize.x * 0.78f, displaySize.y * 0.82f),
            ImGuiCond_Once);
    }
    {
        static bool s_winPosSet = false;
        if (!s_winPosSet) {
            ImVec2 winPos = (g_config.winPosX >= 0.0f && g_config.winPosY >= 0.0f)
                ? ImVec2(g_config.winPosX, g_config.winPosY)
                : ImVec2(displaySize.x * 0.11f, displaySize.y * 0.09f);
            ImGui::SetNextWindowPos(winPos, ImGuiCond_Always);
            s_winPosSet = true;
        }
    }
    //
        float titleLum = 0.299f*g_customThemeCol.x + 0.587f*g_customThemeCol.y + 0.114f*g_customThemeCol.z;
    float textLum  = 0.299f*g_customTextCol.x  + 0.587f*g_customTextCol.y  + 0.114f*g_customTextCol.z;
    ImVec4 mpTitleCol = (titleLum > 0.95f && textLum > 0.95f)
        ? ImVec4(0.40f, 0.40f, 0.40f, 1.0f)
        : g_customTextCol;
    ImGui::PushStyleColor(ImGuiCol_Text, mpTitleCol);
    extern bool g_bLockWindow;
    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (g_bLockWindow) wflags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (!ImGui::Begin("Music Player", p_open, wflags)) {
        ImGui::PopStyleColor();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::PopStyleColor();

    
    extern bool g_bSnowEffect;
    if (g_bSnowEffect) {
        DrawSnowEffect(dt, ImGui::GetWindowPos(), ImGui::GetWindowSize(), ImGui::GetWindowDrawList());
    }

    float fullWidth = ImGui::GetContentRegionAvail().x;
    ImGuiStyle& style = ImGui::GetStyle();

    ImVec4 dimText    = ImVec4(style.Colors[ImGuiCol_Text].x, style.Colors[ImGuiCol_Text].y, style.Colors[ImGuiCol_Text].z, style.Colors[ImGuiCol_Text].w * 0.65f);
    ImVec4 brightText = style.Colors[ImGuiCol_Text];
    ImVec4 highlightText = style.Colors[ImGuiCol_SliderGrabActive]; // Highlight text color based on active theme

    // ── NOW PLAYING (always visible above tabs) ───────────────
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, dimText);
    ImGui::Text("NOW PLAYING");
    ImGui::PopStyleColor();

    {
        float regionH = ImGui::GetTextLineHeightWithSpacing() * 1.1f; 
        ImGui::BeginChild("##NowPlaying", ImVec2(fullWidth, regionH), false,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (g_currentTrack >= 0 && g_currentTrack < (int)g_tracks.size()) {
            std::string name = g_tracks[g_currentTrack].filename;
            size_t dot = name.rfind('.');
            if (dot != std::string::npos) name = name.substr(0, dot);

            float tw  = ImGui::CalcTextSize(name.c_str()).x;
            float aw  = ImGui::GetContentRegionAvail().x;
            ImGui::PushStyleColor(ImGuiCol_Text, brightText);
            if (tw > aw) {
                float loopW  = tw + 80.0f;
                float offset = fmodf((float)ImGui::GetTime() * 60.0f, loopW);
                ImVec2 pos   = ImGui::GetCursorScreenPos();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(pos, ImVec2(pos.x + aw, pos.y + ImGui::GetTextLineHeight() * 1.1f), true);
                dl->AddText(ImVec2(pos.x - offset, pos.y),           ImGui::GetColorU32(brightText), name.c_str());
                if (offset > loopW - aw)
                    dl->AddText(ImVec2(pos.x - offset + loopW, pos.y), ImGui::GetColorU32(brightText), name.c_str());
                dl->PopClipRect();
            } else {
                ImGui::TextUnformatted(name.c_str());
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, dimText);
            ImGui::Text("No track selected");
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 10.0f); 

    // ── TAB BAR ───────────────────────────────────────────────
    static int g_activeTab = 0; // 0 = Main, 1 = Settings

    {
        float tabW = (fullWidth - ScaleX(16.0f)) * 0.5f;   
        float tabH = ScaleY(56.0f);                        
        float tabGap = ScaleX(16.0f);                      

        ImVec4 tabTextActive = (titleLum > 0.95f && 
            (0.299f*g_customTextCol.x + 0.587f*g_customTextCol.y + 0.114f*g_customTextCol.z) > 0.95f)
            ? ImVec4(0.40f, 0.40f, 0.40f, 1.0f)  // gray ONLY when text is also near-white
            : g_customTextCol;                      // otherwise use custom text color
        ImVec4 tabTextInactive = g_customTextCol;
                // ── GET THE DYNAMIC SLIDER ALPHA VALUE ──
        float touchAlpha = style.Colors[ImGuiCol_WindowBg].w;

        // Main tab
        bool mainActive = (g_activeTab == 0);
        
        // Prepare active and inactive colors that perfectly preserve your alpha slider
        ImVec4 mainActiveCol = ImVec4(style.Colors[ImGuiCol_CheckMark].x, style.Colors[ImGuiCol_CheckMark].y, style.Colors[ImGuiCol_CheckMark].z, touchAlpha);
        ImVec4 mainNormalCol = style.Colors[ImGuiCol_Button];

        ImGui::PushStyleColor(ImGuiCol_Button,
            mainActive ? mainActiveCol : mainNormalCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            mainActive ? mainActiveCol : mainActiveCol); // Forced to touchAlpha when hovering active or inactive tabs
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            mainActive ? mainActiveCol : mainActiveCol);
        ImGui::PushStyleColor(ImGuiCol_Text, mainActive ? tabTextActive : tabTextInactive);

        if (ImGui::Button("Main", ImVec2(tabW, tabH)))
            g_activeTab = 0;

        ImGui::PopStyleColor(4);
        ImGui::SameLine(0, tabGap);

        // Settings tab
        bool setActive = (g_activeTab == 1);
        
        ImVec4 setActiveCol = ImVec4(style.Colors[ImGuiCol_CheckMark].x, style.Colors[ImGuiCol_CheckMark].y, style.Colors[ImGuiCol_CheckMark].z, touchAlpha);
        ImVec4 setNormalCol = style.Colors[ImGuiCol_Button];

        ImGui::PushStyleColor(ImGuiCol_Button,
            setActive ? setActiveCol : setNormalCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            setActive ? setActiveCol : setActiveCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            setActive ? setActiveCol : setActiveCol);
        ImGui::PushStyleColor(ImGuiCol_Text, setActive ? tabTextActive : tabTextInactive); 

        if (ImGui::Button("Settings", ImVec2(tabW, tabH)))
            g_activeTab = 1;

        ImGui::PopStyleColor(4);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ═════════════════════════════════════════════════════════
    //  MAIN TAB
    // ═════════════════════════════════════════════════════════
    if (g_activeTab == 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, g_customTextCol);
    // ── Visualizer ────────────────────────────────────────
{
    float visH = ScaleY(110.0f);
    ImVec2      basePos = ImGui::GetCursorScreenPos();
    ImDrawList* dl      = ImGui::GetWindowDrawList();

    // 1. Declare visAlpha SAFELY at the top of the outer block
    ImGuiStyle& style = ImGui::GetStyle();
    float visAlpha = style.Colors[ImGuiCol_WindowBg].w;
    
    ImU32 visBgColor = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_FrameBg]);
    dl->AddRectFilled(basePos, ImVec2(basePos.x + fullWidth, basePos.y + visH), visBgColor, 6.0f);

    ImVec4 activeThemeColor  = style.Colors[ImGuiCol_SliderGrab];
    ImVec4 activeThemeColor2 = ImVec4(
        activeThemeColor.x * 1.3f > 1.0f ? 1.0f : activeThemeColor.x * 1.3f,
        activeThemeColor.y * 1.3f > 1.0f ? 1.0f : activeThemeColor.y * 1.3f,
        activeThemeColor.z * 1.3f > 1.0f ? 1.0f : activeThemeColor.z * 1.3f,
        activeThemeColor.w);

    if (g_visualizerType == 0) // ── Fluid Waveform (Customizable Glow) ──────────────
    {
        float barW = (fullWidth - ScaleX(2.0f) * (VIS_BARS - 1)) / VIS_BARS;
        if (barW < 2.0f) barW = 2.0f;
        float gap  = (fullWidth - barW * VIS_BARS) / (float)(VIS_BARS - 1);
        if (gap < 1.0f) gap = 1.0f;

        // 1. Capture your live theme transparency slider position
        ImGuiStyle& style = ImGui::GetStyle();
        float visAlpha = style.Colors[ImGuiCol_WindowBg].w;

        for (int i = 0; i < VIS_BARS; i++) {
            float h  = g_visBars[i] * visH;
            float x0 = basePos.x + i * (barW + gap);
            float y1 = basePos.y + visH;
            float y0 = y1 - h;

            // 2. Original dynamic brightness math factors
            float brightFactorTop = (120.0f + 80.0f * g_visBars[i]) / 255.0f;
            float brightFactorBot = (200.0f + 55.0f * g_visBars[i]) / 255.0f;

            // 3. MULTIPLY BY ACTIVE THEME RGB
            // This tints the high-brightness formulas with whatever theme color is active!
            float rT = std::min(activeThemeColor.x * (0.4f + brightFactorTop), 1.0f);
            float gT = std::min(activeThemeColor.y * (0.6f + brightFactorTop), 1.0f);
            float bT = std::min(activeThemeColor.z * (0.9f + brightFactorTop), 1.0f);

            float rB = std::min(activeThemeColor.x * (0.2f + brightFactorBot), 1.0f);
            float gB = std::min(activeThemeColor.y * (0.8f + brightFactorBot), 1.0f);
            float bB = std::min(activeThemeColor.z * (0.9f + brightFactorBot), 1.0f);

            // 4. Construct final layout bound directly to your alpha slider context
            ImU32 cT = ImGui::ColorConvertFloat4ToU32(ImVec4(rT, gT, bT, 0.90f * visAlpha));
            ImU32 cB = ImGui::ColorConvertFloat4ToU32(ImVec4(rB, gB, bB, 0.78f * visAlpha));

            // Render the main visualizer frame spectrum columns
            dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x0 + barW, y1), cT, cT, cB, cB);
            
            // 5. Dynamic Reflection layer mapping
            float rh = h * 0.28f;
            ImU32 reflectBg = ImGui::ColorConvertFloat4ToU32(ImVec4(
                activeThemeColor.x * 0.15f, activeThemeColor.y * 0.30f, activeThemeColor.z * 0.60f, 0.07f * visAlpha
            ));
            
            // ── FIXED REMOVED THE HARDCODED BITMASK LINE ──
            // We use standard float conversions tied directly to visAlpha (0.20f * visAlpha)
            // instead of a raw hex bit injection. Now it disappears perfectly!
            ImU32 reflectGlowCol = ImGui::ColorConvertFloat4ToU32(ImVec4(rT, gT, bT, 0.20f * visAlpha));

            dl->AddRectFilledMultiColor(ImVec2(x0, basePos.y), ImVec2(x0 + barW, basePos.y + rh),
                                        reflectBg, reflectBg,
                                        reflectGlowCol, reflectGlowCol);
        }
    }

    
    else if (g_visualizerType == 1) // ── Stereo Spectrum ────────────────────
    {
        // Mirror left/right from center
        float barW = (fullWidth - ScaleX(2.0f) * (VIS_BARS - 1)) / VIS_BARS;
        if (barW < 2.0f) barW = 2.0f;
        float gap  = (fullWidth - barW * VIS_BARS) / (float)(VIS_BARS - 1);
        if (gap < 1.0f) gap = 1.0f;

        // ── FIXED STEREO SPECTRUM ALPHA TO FULL BRIGHTNESS ──
        // Overriding the alpha components with a solid 1.0f keeps the spectrum 
        // fully opaque and colorful while retaining complete RGB customization!
        ImU32 cT = ImGui::ColorConvertFloat4ToU32(ImVec4(
            activeThemeColor2.x, activeThemeColor2.y, activeThemeColor2.z, 1.0f));
            
        ImU32 cB = ImGui::ColorConvertFloat4ToU32(ImVec4(
            activeThemeColor.x, activeThemeColor.y, activeThemeColor.z, 1.0f));

        for (int i = 0; i < VIS_BARS; i++) {
            // Mirror: left half mirrors right half
            int srcIdx = (i < VIS_BARS / 2) ? (VIS_BARS / 2 - 1 - i) : (i - VIS_BARS / 2);
            float h  = g_visBars[srcIdx] * visH;
            float x0 = basePos.x + i * (barW + gap);
            float y1 = basePos.y + visH;
            float y0 = y1 - h;
            dl->AddRectFilledMultiColor(ImVec2(x0,y0), ImVec2(x0+barW,y1), cT, cT, cB, cB);
        }
    }

    
    //
    else if (g_visualizerType == 2) // ── Digital Blocks (LED grid) ──────────
{
    const int   COLS    = VIS_BARS;
    const int   ROWS    = 14;
    const float padding = ScaleX(1.8f);

    float cellW = (fullWidth - padding * (COLS + 1)) / COLS;
    float cellH = (visH      - padding * (ROWS + 1)) / ROWS;
    if (cellW < 1.0f) cellW = 1.0f;
    if (cellH < 1.0f) cellH = 1.0f;
    float rounding = ScaleX(1.2f);

    ImVec4 tc  = activeThemeColor;
    ImVec4 tc2 = activeThemeColor2;

    // ── GET THE DYNAMIC SLIDER ALPHA VALUE ──
    ImGuiStyle& style = ImGui::GetStyle();
    float visAlpha = style.Colors[ImGuiCol_WindowBg].w;

    // ── FIXED DIGITAL BLOCKS ALPHA ──
    // We explicitly overwrite or scale the alpha channel ('w') of every single block state
    // with our transparent slider's alpha value (visAlpha).
    ImU32 colLow  = ImGui::ColorConvertFloat4ToU32(ImVec4(
        tc.x * 0.55f, tc.y * 0.55f, tc.z * 0.55f, visAlpha));
        
    ImU32 colMid  = ImGui::ColorConvertFloat4ToU32(ImVec4(
        tc.x, tc.y, tc.z, visAlpha));
        
    ImU32 colTop  = ImGui::ColorConvertFloat4ToU32(ImVec4(
        tc2.x, tc2.y, tc2.z, visAlpha));
        
    // Off-state blocks blend with background panel transparency
    ImU32 colOff  = ImGui::ColorConvertFloat4ToU32(ImVec4(
        tc.x * 0.10f, tc.y * 0.10f, tc.z * 0.10f, 0.45f * visAlpha));
        
    ImU32 colPeak = ImGui::ColorConvertFloat4ToU32(ImVec4(
        tc2.x, tc2.y, tc2.z, visAlpha));

    for (int col = 0; col < COLS; col++) {
        int litRows = (int)(g_visBars[col] * ROWS + 0.5f);
        int peakRow = (int)(g_visPeak[col] * ROWS + 0.5f);
        if (litRows > ROWS) litRows = ROWS;
        if (peakRow > ROWS) peakRow = ROWS;

        float x0 = basePos.x + padding + col * (cellW + padding);

        for (int row = 0; row < ROWS; row++) {
            float y0 = basePos.y + padding + row * (cellH + padding);
            ImVec2 rMin(x0, y0);
            ImVec2 rMax(x0 + cellW, y0 + cellH);

            int  rowFromBottom = ROWS - 1 - row;
            bool lit  = (rowFromBottom < litRows);
            bool peak = (rowFromBottom == peakRow - 1 && peakRow > 0 && !lit);

            if (peak) {
                dl->AddRectFilled(rMin, rMax, colPeak, rounding);
            } else if (!lit) {
                dl->AddRectFilled(rMin, rMax, colOff, rounding);
            } else if (rowFromBottom >= litRows - 2) {
                dl->AddRectFilled(rMin, rMax, colTop, rounding);
            } else if (rowFromBottom >= litRows / 2) {
                dl->AddRectFilled(rMin, rMax, colMid, rounding);
            } else {
                dl->AddRectFilled(rMin, rMax, colLow, rounding);
            }
        }
    }
}
    
    // ── 7 - Line Bar Visualizer ──────────────────────────────────────────
    else if (g_visualizerType == 3)
{
    float midY = basePos.y + (visH / 2.0f);
    
    // 1. Choose your desired tight gap spacing between the bars
    float customGap = ScaleX(3.5f);

    // 2. Dynamically calculate bar width so the entire group stretches perfectly edge-to-edge
    float customBarW = (fullWidth - (customGap * (VIS_BARS - 1))) / (float)VIS_BARS;
    if (customBarW < 1.0f) customBarW = 1.0f; // Safety guard for extremely small resolutions
    
    // 3. Start exactly at the left boundary edge
    float startX = basePos.x;

    ImU32 splitBarCol = ImGui::ColorConvertFloat4ToU32(ImVec4(activeThemeColor.x, activeThemeColor.y, activeThemeColor.z, 0.90f * visAlpha));

    for (int i = 0; i < VIS_BARS; i++) {
        float halfH = (g_visBars[i] * visH * 0.45f);
        
        // 4. Spread bars across the entire calculated width bounds
        float x0    = startX + i * (customBarW + customGap);
        float x1    = x0 + customBarW;

        dl->AddRectFilled(ImVec2(x0, midY - halfH), ImVec2(x1, midY), splitBarCol, 1.0f);
        dl->AddRectFilled(ImVec2(x0, midY), ImVec2(x1, midY + halfH), splitBarCol, 1.0f);
    }
}

        // ── 8 - Line Visualizer ──────────────────────────────────────────────
        else if (g_visualizerType == 4)
        {
            float midY = basePos.y + (visH / 2.0f);
            float stepX = fullWidth / (float)(VIS_BARS - 1);
            ImU32 waveLineCol = ImGui::ColorConvertFloat4ToU32(ImVec4(activeThemeColor.x, activeThemeColor.y, activeThemeColor.z, 0.95f * visAlpha));

            ImVec2 prevPoint;
            for (int i = 0; i < VIS_BARS; i++) {
                float offset = g_visBars[i] * (visH * 0.45f);
                float currentY = (i % 2 == 0) ? (midY - offset) : (midY + offset);
                ImVec2 currentPoint = ImVec2(basePos.x + (i * stepX), currentY);

                if (i > 0) {
                    dl->AddLine(prevPoint, currentPoint, waveLineCol, 2.5f);
                }
                prevPoint = currentPoint;
            }
        }
        // ── 5 - Lyrics Only ──────────────────────────────────────────────────────────
        else if (g_visualizerType == 5)
        {
            float midY    = basePos.y + (visH / 2.0f);
            float centerX = basePos.x + (fullWidth / 2.0f);

            if (!g_currentLyricsLineText.empty())
            {
                ImU32 lyricCol  = ImGui::ColorConvertFloat4ToU32(ImVec4(
                    activeThemeColor.x, activeThemeColor.y, activeThemeColor.z, 0.95f * visAlpha));
                ImU32 shadowCol = ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, 0.60f * visAlpha));

                float maxWidth = fullWidth - ScaleX(20.0f);
                ImVec2 textSize = ImGui::CalcTextSize(g_currentLyricsLineText.c_str());

                if (textSize.x <= maxWidth) {
                    // Single line
                    ImVec2 textPos = ImVec2(centerX - (textSize.x / 2.0f), midY - (textSize.y / 2.0f));
                    dl->AddText(ImVec2(textPos.x + ScaleX(1.5f), textPos.y + ScaleY(1.5f)), shadowCol, g_currentLyricsLineText.c_str());
                    dl->AddText(ImVec2(textPos.x + ScaleX(1.5f), textPos.y + ScaleY(1.5f)), shadowCol, g_currentLyricsLineText.c_str());
                    dl->AddText(textPos, lyricCol, g_currentLyricsLineText.c_str());
                } else {
                    // Split into two lines at nearest space to middle
                    const std::string& txt = g_currentLyricsLineText;
                    size_t mid = txt.size() / 2;
                    size_t splitPos = mid;
                    for (size_t d = 0; d < mid; d++) {
                        if (txt[mid - d] == ' ') { splitPos = mid - d; break; }
                        if (mid + d < txt.size() && txt[mid + d] == ' ') { splitPos = mid + d; break; }
                    }
                    std::string line1 = txt.substr(0, splitPos);
                    std::string line2 = txt.substr(splitPos + 1);

                    ImVec2 s1 = ImGui::CalcTextSize(line1.c_str());
                    ImVec2 s2 = ImGui::CalcTextSize(line2.c_str());
                    float lineSpacing = ImGui::GetTextLineHeightWithSpacing();

                    ImVec2 p1 = ImVec2(centerX - s1.x / 2.0f, midY - lineSpacing);
                    ImVec2 p2 = ImVec2(centerX - s2.x / 2.0f, midY);

                    dl->AddText(ImVec2(p1.x + ScaleX(1.5f), p1.y + ScaleY(1.5f)), shadowCol, line1.c_str());
                    dl->AddText(p1, lyricCol, line1.c_str());
                    dl->AddText(ImVec2(p2.x + ScaleX(1.5f), p2.y + ScaleY(1.5f)), shadowCol, line2.c_str());
                    dl->AddText(p2, lyricCol, line2.c_str());
                }
            }
            else
            {
                const char* fallbackText = g_isPlaying ? "Instrumental / No Lyrics Found" : "Playback Stopped";
                ImVec2 textSize = ImGui::CalcTextSize(fallbackText);
                ImU32 fallbackCol = ImGui::ColorConvertFloat4ToU32(ImVec4(0.5f, 0.5f, 0.5f, 0.45f * visAlpha));
                dl->AddText(ImVec2(centerX - (textSize.x / 2.0f), midY - (textSize.y / 2.0f)), fallbackCol, fallbackText);
            }
        }

    // ── 5 - Off State ────────────────────────────────────────────────────────────
    else if (g_visualizerType == 6)
    {
       // Intentionally empty: visualizer rendering turns completely off.
    }

    ImGui::Dummy(ImVec2(fullWidth, visH + ScaleY(4.0f)));
}

        ImGui::Spacing();

        // ── Seek bar ──────────────────────────────────────────
        {
            float progress = GetProgress();
            float posSec   = GetPositionSec();
            float durSec   = GetDurationSec();
            char sCur[16], sTot[16];
            FormatTime(sCur, sizeof(sCur), posSec);
            FormatTime(sTot, sizeof(sTot), durSec);

            ImVec2      pbPos = ImGui::GetCursorScreenPos();
            float       pbH   = ScaleY(18.0f);
            ImDrawList* dl    = ImGui::GetWindowDrawList();

            // FIX: Dynamically generated track baseline background using FrameBg elements to prevent the default dark blue sheen leakage
            ImU32 trackBg = ImGui::ColorConvertFloat4ToU32(ImVec4(style.Colors[ImGuiCol_FrameBg].x, style.Colors[ImGuiCol_FrameBg].y, style.Colors[ImGuiCol_FrameBg].z, style.Colors[ImGuiCol_FrameBg].w * 0.8f));
            ImU32 fillCol = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_SliderGrab]);
            ImU32 grabCol = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_SliderGrab]);

            dl->AddRectFilled(pbPos, ImVec2(pbPos.x + fullWidth, pbPos.y + pbH), trackBg, 4.0f);
            if (progress > 0.0f && g_soundLoaded)
                dl->AddRectFilled(pbPos, ImVec2(pbPos.x + fullWidth * progress, pbPos.y + pbH), fillCol, 4.0f);
            if (g_soundLoaded) {
                float tx = pbPos.x + fullWidth * progress;
                dl->AddCircleFilled(ImVec2(tx, pbPos.y + pbH * 0.5f), pbH * 0.78f, grabCol);
            }
            ImGui::SetCursorScreenPos(pbPos);
            ImGui::InvisibleButton("##seekbar", ImVec2(fullWidth, pbH));
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
                float mx = ImGui::GetIO().MousePos.x;
                SeekToProgress((mx - pbPos.x) / fullWidth);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, dimText);
            ImGui::Text("%s", sCur);
            ImGui::SameLine(fullWidth - ScaleX(90.0f));
            ImGui::Text("%s", sTot);
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();

        // ── Transport buttons ─────────────────────────────────
        {
            float btnH  = ScaleY(72.0f);
            float btnW5 = (fullWidth - ScaleX(16.0f) * 4.0f) / 5.0f;
            float sp    = ScaleX(16.0f);

            // ── GET THE DYNAMIC SLIDER ALPHA VALUE ──
            float touchAlpha = style.Colors[ImGuiCol_WindowBg].w;

            if (MusicButton("PREV", ImVec2(btnW5, btnH))) PlayPrev();
            ImGui::SameLine(0, sp);

            if (MusicButton(g_isPlaying ? "PAUSE" : "PLAY ", ImVec2(btnW5, btnH))) TogglePlayPause();
            ImGui::SameLine(0, sp);

            // ── FIXED STOP BUTTON ALPHA ──
            // Injecting touchAlpha directly into the red stop button color channels
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.60f, 0.12f, 0.12f, touchAlpha));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, touchAlpha));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.45f, 0.08f, 0.08f, touchAlpha)); 
            if (ImGui::Button("STOP", ImVec2(btnW5, btnH))) StopPlayback();
            ImGui::PopStyleColor(3);
            ImGui::SameLine(0, sp);

            if (MusicButton("NEXT", ImVec2(btnW5, btnH))) PlayNext();
            ImGui::SameLine(0, sp);

            // ── FIXED RLD BUTTON ALPHA ──
            // Injecting touchAlpha directly into the green reload button color channels
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.40f, 0.15f, touchAlpha));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.55f, 0.22f, touchAlpha));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.10f, 0.30f, 0.10f, touchAlpha)); 
            if (ImGui::Button("RLD", ImVec2(btnW5, btnH))) { StopPlayback(); ScanMusicFolder(); }
            ImGui::PopStyleColor(3);
        }

        ImGui::Spacing();

        
        // ── Loop / Shuffle ────────────────────────────────────
        {
            float btnW = (fullWidth - ScaleX(16.0f)) * 0.5f; 
            float btnH = ScaleY(56.0f);                      

            float _tLum = 0.299f*g_customThemeCol.x + 0.587f*g_customThemeCol.y + 0.114f*g_customThemeCol.z;
            float _xLum = 0.299f*g_customTextCol.x  + 0.587f*g_customTextCol.y  + 0.114f*g_customTextCol.z;
            ImU32 iconColActive   = (_tLum > 0.95f && _xLum > 0.95f)
                ? IM_COL32(100, 100, 100, 255)
                : ImGui::ColorConvertFloat4ToU32(g_customTextCol);
            ImU32 iconColInactive = ImGui::ColorConvertFloat4ToU32(g_customTextCol);
            
                        // --- LOOP BUTTON ---
            ImGui::PushID("LoopBtn");
                        
            if (g_bLoop) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            }
            
            
            if (ImGui::Button("##Loop", ImVec2(btnW, btnH))) {
                g_bLoop = !g_bLoop;
                if (g_soundLoaded) {
                    ma_sound_set_looping(&g_sound, g_bLoop ? MA_TRUE : MA_FALSE);
                }
            }

            // Draw the precise loop track and indicator dot matching your reference image
            DrawLoopIcon(
                ImGui::GetWindowDrawList(), 
                ImGui::GetItemRectMin(), 
                ImGui::GetItemRectMax(), 
                g_bLoop ? iconColActive : iconColInactive,
                g_bLoop
            );
            ImGui::PopStyleColor();
            ImGui::PopID();

            ImGui::SameLine(0, ScaleX(16.0f));

            // --- SHUFFLE BUTTON ---
            ImGui::PushID("ShuffleBtn");
                        
            if (g_bShuffle) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            }
            
            
            if (ImGui::Button("##Shuffle", ImVec2(btnW, btnH))) {
                g_bShuffle = !g_bShuffle;
            }

            // Draw the segment-crossing shuffle arrows and indicator dot matching your reference image
            DrawShuffleIcon(
                ImGui::GetWindowDrawList(), 
                ImGui::GetItemRectMin(), 
                ImGui::GetItemRectMax(), 
                g_bShuffle ? iconColActive : iconColInactive,
                g_bShuffle 
            );
            ImGui::PopStyleColor();
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

                
        // ── Track list ────────────────────────────────────────
        
                        // ── Track list ────────────────────────────────────────
        static char g_searchBuf[128] = "";
        ImGuiStyle& style = ImGui::GetStyle();
        float fullWidth = ImGui::GetContentRegionAvail().x;
        float tw = (fullWidth - ScaleX(16.0f)) * 0.5f;

        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        ImGui::Text("LIBRARY  (%zu tracks)", g_tracks.size());
        ImGui::PopStyleColor();
        ImGui::SameLine();

        float rightAlignmentPosX = ImGui::GetWindowWidth() - tw - style.WindowPadding.x;
        ImGui::SetCursorPosX(rightAlignmentPosX);

        // Manual draw — no InputTextWithHint so text color never changes on click
        ImVec2 boxMin = ImGui::GetCursorScreenPos();
        float boxH = ImGui::GetFrameHeight();
        ImVec2 boxMax = ImVec2(boxMin.x + tw, boxMin.y + boxH);

        ImGui::GetWindowDrawList()->AddRectFilled(boxMin, boxMax,
            ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_FrameBg]), style.FrameRounding);
        ImVec4 borderCol = g_bShowKeyboard ? style.Colors[ImGuiCol_SliderGrab] : style.Colors[ImGuiCol_Border];
        ImGui::GetWindowDrawList()->AddRect(boxMin, boxMax,
            ImGui::ColorConvertFloat4ToU32(borderCol), style.FrameRounding, 0, g_bShowKeyboard ? 2.0f : 1.0f);

        std::string displayText = (strlen(g_searchBuf) == 0) ? "Search" : g_searchBuf;
        ImVec4 textCol = (strlen(g_searchBuf) == 0) ? dimText : g_customTextCol;
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(boxMin.x + style.FramePadding.x, boxMin.y + style.FramePadding.y),
            ImGui::ColorConvertFloat4ToU32(textCol), displayText.c_str());

        ImGui::InvisibleButton("##SearchBox", ImVec2(tw, boxH));
        if (ImGui::IsItemClicked()) g_bShowKeyboard = true;

        if (g_bShowKeyboard) {
            float cursorTimer = fmodf((float)ImGui::GetTime(), 1.0f);
            if (cursorTimer < 0.5f) {
                float textW = ImGui::CalcTextSize(g_searchBuf).x;
                float cursorX = boxMin.x + style.FramePadding.x + textW + 1.0f;
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(cursorX, boxMin.y + style.FramePadding.y),
                    ImVec2(cursorX, boxMax.y - style.FramePadding.y),
                    ImGui::ColorConvertFloat4ToU32(g_customTextCol), 1.5f);
            }
        }

        ImGui::Spacing();

        if (g_bShowKeyboard) {
            DrawImGuiKeyboard(g_searchBuf, sizeof(g_searchBuf));
        }
        
                
                
        float listH             = ImGui::GetContentRegionAvail().y - ScaleY(8.0f);
        if (listH < ScaleY(40.0f)) listH = ScaleY(40.0f);

        float sliderTrackWidth  = ScaleX(55.0f); // 45.0 adjust thickness here
        float listContainerWidth= fullWidth - sliderTrackWidth - style.WindowPadding.x;
        float rowH              = ScaleY(52.0f);
        
        static std::string lastSearch = "";
        std::vector<int> filteredIdx;
        {
            std::string lowerSearch = g_searchBuf;
            std::string searchLower = lowerSearch;
            for (char& c : searchLower) if ((unsigned char)c < 128) c = (char)tolower(c);
            for (int i = 0; i < (int)g_tracks.size(); i++) {
                if (searchLower.empty()) { filteredIdx.push_back(i); continue; }
                std::string name = g_tracks[i].filename;
                std::string nameLower = name;
                for (char& c : nameLower) if ((unsigned char)c < 128) c = (char)tolower(c);
                if (nameLower.find(searchLower) != std::string::npos) filteredIdx.push_back(i);
            }
            if (searchLower != lastSearch) {
                lastSearch = searchLower;
                if (g_currentTrack >= 0) g_scrollToTrack = g_currentTrack;
            }
            g_filteredIdx = filteredIdx;
        }
                            //
        int   trackCount        = (int)filteredIdx.size();
        
        float totalContentH     = trackCount * rowH;
        float maxScrollY        = std::max(0.0f, totalContentH - listH);        
        
        if (g_scrollToTrack >= 0 && trackCount > 0) {
            // Find position of g_scrollToTrack within filteredIdx
            int filteredPos = -1;
            for (int i = 0; i < (int)filteredIdx.size(); i++) {
                if (filteredIdx[i] == g_scrollToTrack) { filteredPos = i; break; }
            }
            if (filteredPos >= 0) {
                float idealTop = (filteredPos * rowH) - (listH * 0.5f) + (rowH * 0.5f);
                g_scrollTarget = idealTop < 0.0f ? 0.0f : (idealTop > maxScrollY ? maxScrollY : idealTop);
            }
            g_scrollToTrack = -1;
        }
        
        
        if (!g_isDraggingSlider && !ImGui::GetIO().MouseDown[0]) {
            float diff = g_scrollTarget - g_dragScrollY;
            if (fabsf(diff) > 0.5f)
                g_dragScrollY += diff * (ImGui::GetIO().DeltaTime * 12.0f);
            else
                g_dragScrollY = g_scrollTarget;
        }

        if (g_isDraggingSlider && maxScrollY > 0.0f) {
            g_dragScrollY  = g_customScrollPercent * maxScrollY;
            g_scrollTarget = g_dragScrollY;
        }

        if (g_dragScrollY < 0.0f)       g_dragScrollY = 0.0f;
        if (g_dragScrollY > maxScrollY) g_dragScrollY = maxScrollY;
        if (!g_isDraggingSlider && maxScrollY > 0.0f)
            g_customScrollPercent = g_dragScrollY / maxScrollY;

        ImVec2 listScreenPos = ImGui::GetCursorScreenPos();
        float  listScreenX   = listScreenPos.x;
        float  listScreenY   = listScreenPos.y;

        ImDrawList* dlBg = ImGui::GetWindowDrawList();
        
        // FIX: Re-mapped the main library box frame background calculation so it stays perfectly aligned with the selected theme tint
        ImU32 containerBg = ImGui::ColorConvertFloat4ToU32(ImVec4(style.Colors[ImGuiCol_FrameBg].x, style.Colors[ImGuiCol_FrameBg].y, style.Colors[ImGuiCol_FrameBg].z, style.Colors[ImGuiCol_FrameBg].w * 0.9f));
                dlBg->AddRectFilled(listScreenPos,
            ImVec2(listScreenX + fullWidth, listScreenY + listH),    
            containerBg, 4.0f);

        ImGuiIO& lio     = ImGui::GetIO();
        ImVec2 mousePos  = lio.MousePos;
        
        bool mouseInList = (mousePos.x >= listScreenX &&
                            mousePos.x <  listScreenX + listContainerWidth &&
                            mousePos.y >= listScreenY &&
                            mousePos.y <  listScreenY + listH);
        
        static bool  s_wasDragging   = false;
        static float s_dragStartY    = 0.0f;
        static float s_scrollAtStart = 0.0f;
        static float s_touchDownY    = 0.0f;
        static bool  s_touchActive   = false;

        bool kbBlockingList = false;
        if (g_bShowKeyboard) {
            if (ImGuiWindow* kbw = ImGui::FindWindowByName("Keyboard")) {
                if (kbw->Active &&
                    mousePos.x >= kbw->Pos.x && mousePos.x <= kbw->Pos.x + kbw->Size.x &&
                    mousePos.y >= kbw->Pos.y && mousePos.y <= kbw->Pos.y + kbw->Size.y)
                    kbBlockingList = true;
            }
        }
        if (mouseInList && lio.MouseClicked[0] && !kbBlockingList) {
            s_touchActive   = true;
            s_wasDragging   = false;
            s_dragStartY    = mousePos.y;
            s_scrollAtStart = g_dragScrollY;
            s_touchDownY    = mousePos.y;
            g_isDraggingSlider = false;
        }
                if (s_touchActive && lio.MouseDown[0]) {
            float deltaY = s_dragStartY - mousePos.y;
            if (fabsf(mousePos.y - s_touchDownY) > ScaleY(6.0f)) s_wasDragging = true;
            if (s_wasDragging) {
                g_dragScrollY = s_scrollAtStart + deltaY;
                if (g_dragScrollY < 0.0f)       g_dragScrollY = 0.0f;
                if (g_dragScrollY > maxScrollY) g_dragScrollY = maxScrollY;
                if (maxScrollY > 0.0f)
                    g_customScrollPercent = g_dragScrollY / maxScrollY;
                g_scrollTarget = g_dragScrollY;
            }
        }
        if (s_touchActive && lio.MouseReleased[0]) {
            if (!s_wasDragging && mouseInList) {
                float relY   = mousePos.y - listScreenY + g_dragScrollY;
                int   tapped = (int)(relY / rowH);
                if (tapped >= 0 && tapped < trackCount) {
                    LoadAndPlay(filteredIdx[tapped]);
                    g_scrollToTrack = filteredIdx[tapped];
                }
            }
            s_touchActive = false;
            s_wasDragging = false;
        }

        if (trackCount > 0) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(ImVec2(listScreenX, listScreenY),
                             ImVec2(listScreenX + listContainerWidth, listScreenY + listH), true);

            ImU32 highlightRowCol = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_Header]);
            // FIX: Altering rows now compute shade multipliers via dynamic active window color nodes instead of static values

            int firstVis = (int)(g_dragScrollY / rowH);
            int lastVis  = (int)((g_dragScrollY + listH) / rowH) + 1;
            if (firstVis < 0)           firstVis = 0;
            if (lastVis >= trackCount)  lastVis  = trackCount - 1;

                
            for (int vi = firstVis; vi <= lastVis; vi++) {
                int    i        = filteredIdx[vi];
                bool   isActive = (i == g_currentTrack);
                float  ry       = listScreenY + (vi * rowH) - g_dragScrollY;
            //
                ImVec2 rMin(listScreenX, ry);
                ImVec2 rMax(listScreenX + listContainerWidth, ry + rowH);

                if (isActive)
                    dl->AddRectFilled(rMin, rMax, highlightRowCol, 4.0f);

                std::string disp = g_tracks[i].filename;
                size_t dot = disp.rfind('.');
                if (dot != std::string::npos) disp = disp.substr(0, dot);
                char rowLabel[256];
                snprintf(rowLabel, sizeof(rowLabel), " %02d  %s", i+1, disp.c_str());

                ImVec2 tPos(rMin.x + ScaleX(6.0f),
                            rMin.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f);

                if (isActive) {
                    float lw = ImGui::CalcTextSize(rowLabel).x;
                    float rw = listContainerWidth;
                    if (lw > rw) {
                        float loopW  = lw + 60.0f;
                        float offset = fmodf((float)ImGui::GetTime() * 45.0f, loopW);
                        // FIX: Changed color implementation to use highlightText (from the theme) instead of regular brightText
                        dl->AddText(ImVec2(tPos.x - offset, tPos.y), ImGui::ColorConvertFloat4ToU32(highlightText), rowLabel);
                        if (offset > loopW - rw)
                            dl->AddText(ImVec2(tPos.x - offset + loopW, tPos.y), ImGui::ColorConvertFloat4ToU32(highlightText), rowLabel);
                    } else {
                        // FIX: Highlights non-scrolling text lines with the theme highlight color instead of white
                        dl->AddText(tPos, ImGui::ColorConvertFloat4ToU32(highlightText), rowLabel);
                    }
                } else {
                    dl->AddText(tPos, ImGui::ColorConvertFloat4ToU32(brightText), rowLabel);
                }
            }
            dl->PopClipRect();
        } else {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(ImVec2(listScreenX + ScaleX(8.0f), listScreenY + ScaleY(10.0f)),
                        ImGui::ColorConvertFloat4ToU32(dimText),
                        "No audio files found. Check music folder and tap RLD.");
        }

        float listCursorY = ImGui::GetCursorPosY();
        ImGui::Dummy(ImVec2(listContainerWidth, listH));

        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - sliderTrackWidth - style.WindowPadding.x);
        ImGui::SetCursorPosY(listCursorY);

        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize,   ScaleY(80.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding,  ScaleX(12.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ScaleX(12.0f));
        
        ImGui::PushStyleColor(ImGuiCol_FrameBg,          style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,       style.Colors[ImGuiCol_ScrollbarGrab]);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, style.Colors[ImGuiCol_ScrollbarGrab]);

        if (ImGui::VSliderFloat("##LibraryScroller", ImVec2(sliderTrackWidth, listH),
                                 &g_customScrollPercent, 1.0f, 0.0f, "")) {
            g_isDraggingSlider = true;
            s_touchActive      = false;
        } else {
            g_isDraggingSlider = false;
        }
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(); // restore text color

    } 

    // ═════════════════════════════════════════════════════════
    //  SETTINGS TAB
    // ═════════════════════════════════════════════════════════
    else if (g_activeTab == 1)
    {
        // ── Visualizer Selection ──────────────────────────────
        static const char* visualizerNames[] = {
            "Fluid Waveform", "Stereo Spectrum", "Digital Blocks", "Line Bar", "Line", "Lyrics", "None"
        };

        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        ImGui::Text("VISUALIZER");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::Text("Current Visualizer");
        ImGui::SameLine(ScaleX(640.0f));
        ImGui::SetNextItemWidth(fullWidth - ScaleX(640.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_PopupBg,        style.Colors[ImGuiCol_WindowBg]);
        ImGui::PushStyleColor(ImGuiCol_Button,         style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  style.Colors[ImGuiCol_CheckMark]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   style.Colors[ImGuiCol_CheckMark]);

        if (ImGui::BeginCombo("##VisualizerCombo", visualizerNames[g_visualizerType])) {
            // from 4 to 6
            for (int i = 0; i < 7; i++) { 
                bool sel = (g_visualizerType == i);
                // FIX: Wrapped Selectable fields inside standard style color push macros so they are readable
                ImGui::PushStyleColor(ImGuiCol_Header,        style.Colors[ImGuiCol_Header]);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, style.Colors[ImGuiCol_HeaderHovered]);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  style.Colors[ImGuiCol_HeaderHovered]);
                if (ImGui::Selectable(visualizerNames[i], sel)) {
                    g_visualizerType = i;
                }
                ImGui::PopStyleColor(3);
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor(6);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Theme selection ───────────────────────────────────
        // g_theme, g_customTextCol, g_customThemeCol, g_customHighlightCol
        // are file-scope globals (promoted for INI config access).
        static const char* themeNames[] = {
            "Default", "Dark Gray", "Old Rose", "Teal", "Purple", "Chili Red", "Custom"
        };

        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        ImGui::Text("THEME");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::Text("Current Theme");
        ImGui::SameLine(ScaleX(640.0f)); 
        ImGui::SetNextItemWidth(fullWidth - ScaleX(640.0f)); 
        int pendingTheme = -1;
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_PopupBg,        style.Colors[ImGuiCol_WindowBg]);
        ImGui::PushStyleColor(ImGuiCol_Button,         style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  style.Colors[ImGuiCol_CheckMark]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   style.Colors[ImGuiCol_CheckMark]);
        
        extern ImVec4 g_customButtonOpenCloseCol; // open/close button

        // ── FIRST RUN INITIALIZATION SYNCHRONIZATION ──
        // Only sync from style when no config was loaded (g_theme == 0 and
        // we have never been through the settings tab before).
                
        static bool isFirstRun = true;
        if (isFirstRun) {
            isFirstRun = false;
            if (g_config.winPosX < 0.0f) {
                // No config file existed — sync custom colors from the active style
                g_customTextCol      = style.Colors[ImGuiCol_Text];
                g_customThemeCol     = style.Colors[ImGuiCol_TitleBgActive];
                g_customHighlightCol = style.Colors[ImGuiCol_CheckMark];
            }
            // If config was loaded, g_custom*Col are already correct from LoadConfig
        }
        
        if (ImGui::BeginCombo("##ThemeCombo", themeNames[g_theme])) {
            for (int i = 0; i < 7; i++) { 
                bool sel = (g_theme == i);
                // FIX: Added Theme Selection Selectable popup context bindings
                ImGui::PushStyleColor(ImGuiCol_Header,        style.Colors[ImGuiCol_Header]);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, style.Colors[ImGuiCol_HeaderHovered]);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  style.Colors[ImGuiCol_HeaderHovered]);
                if (ImGui::Selectable(themeNames[i], sel)) {
                    g_theme = i;
                    pendingTheme = i;
                }
                ImGui::PopStyleColor(3);
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }       

        ImGui::PopStyleColor(6);
        // Apply theme colors AFTER popping FrameBg/FrameBgHovered/PopupBg pushes above,
        // so the new colors aren't immediately overwritten by PopStyleColor.
        if (pendingTheme >= 0) {
            ApplyThemeToStyle(pendingTheme);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();       

        // ── Custom theme color pickers ────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        ImGui::Text("CUSTOM THEME");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        
        ImGui::SetNextItemWidth(fullWidth - ScaleX(220.0f));
        if (ImGui::ColorEdit4("Text", (float*)&g_customTextCol, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_AlphaPreview)) {
            g_theme = 6;
            ApplyThemeToStyle(6);
        }
                // this
        ImGui::SetNextItemWidth(fullWidth - ScaleX(220.0f));
        if (ImGui::ColorEdit4("Theme", (float*)&g_customThemeCol, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_AlphaPreview)) {
            g_theme = 6;
            ImGuiStyle& s = ImGui::GetStyle();

            // 1. Capture the exact custom alpha slider value
            float a = g_customThemeCol.w;

            float minBright = 0.17f; // 0.08
            float tr = std::max(g_customThemeCol.x, minBright);
            float tg = std::max(g_customThemeCol.y, minBright);
            float tb = std::max(g_customThemeCol.z, minBright);

            float lum2 = 0.299f*tr + 0.587f*tg + 0.114f*tb;
            ImVec4 baseCol, touchCol;
            if (lum2 > 0.6f) {
                baseCol  = ImVec4(tr * 0.6f, tg * 0.6f, tb * 0.6f, a);
                touchCol = ImVec4(tr, tg, tb, a);
            } else {
                baseCol  = ImVec4(tr, tg, tb, a);
                touchCol = ImVec4(
                    std::min(tr * 1.4f, 1.0f),
                    std::min(tg * 1.4f, 1.0f),
                    std::min(tb * 1.4f, 1.0f), a);
            }
            
            float bgMin = 0.02f;
            s.Colors[ImGuiCol_TitleBgActive]    = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
            s.Colors[ImGuiCol_TitleBg]          = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
            s.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a * 0.5f);
            s.Colors[ImGuiCol_Border]           = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), 0.80f);
            
            s.Colors[ImGuiCol_CheckMark]        = ImVec4(touchCol.x, touchCol.y, touchCol.z, s.Colors[ImGuiCol_Text].w); 
            
            s.Colors[ImGuiCol_SliderGrab]           = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
            s.Colors[ImGuiCol_ScrollbarGrab]        = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
            s.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
            s.Colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);

            s.Colors[ImGuiCol_ScrollbarBg]      = ImVec4(std::max(tr*0.30f, bgMin*0.5f), std::max(tg*0.30f, bgMin*0.5f), std::max(tb*0.30f, bgMin*0.5f), a);
            s.Colors[ImGuiCol_WindowBg]         = ImVec4(std::max(tr*0.80f, bgMin), std::max(tg*0.82f, bgMin), std::max(tb*0.82f, bgMin), a*0.95f);
            s.Colors[ImGuiCol_FrameBg]          = ImVec4(std::max(tr*0.60f, bgMin), std::max(tg*0.60f, bgMin), std::max(tb*0.60f, bgMin), a);
            s.Colors[ImGuiCol_FrameBgHovered]   = ImVec4(std::max(tr*0.75f, bgMin), std::max(tg*0.75f, bgMin), std::max(tb*0.75f, bgMin), a);
            s.Colors[ImGuiCol_FrameBgActive]    = s.Colors[ImGuiCol_FrameBg];

            s.Colors[ImGuiCol_Button]           = baseCol;
            s.Colors[ImGuiCol_ButtonHovered]    = touchCol;
            s.Colors[ImGuiCol_ButtonActive]     = touchCol;
            s.Colors[ImGuiCol_Header]           = s.Colors[ImGuiCol_Button];
            s.Colors[ImGuiCol_HeaderHovered]    = s.Colors[ImGuiCol_ButtonHovered];

            float mixFactor = (lum2 < 0.3f) ? 0.40f : 0.15f; 
            s.Colors[ImGuiCol_Separator] = ImVec4(
                std::max(tr*(1.0f-mixFactor)+(1.0f*mixFactor), bgMin),
                std::max(tg*(1.0f-mixFactor)+(1.0f*mixFactor), bgMin),
                std::max(tb*(1.0f-mixFactor)+(1.0f*mixFactor), bgMin),
                0.80f);
            
            s.Colors[ImGuiCol_ResizeGrip]       = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a*0.6f);
            s.Colors[ImGuiCol_ResizeGripHovered]= s.Colors[ImGuiCol_ButtonHovered];
            s.Colors[ImGuiCol_ResizeGripActive] = ImVec4(std::max(tr, bgMin), std::max(tg, bgMin), std::max(tb, bgMin), a);
        }

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ScaleY(4.0f));

        ImGui::SetNextItemWidth(fullWidth - ScaleX(220.0f));
        if (ImGui::ColorEdit4("Highlight", (float*)&g_customHighlightCol, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_AlphaPreview)) {
            g_theme = 6;
            ImGuiStyle& s = ImGui::GetStyle();
            s.Colors[ImGuiCol_SliderGrabActive] = g_customHighlightCol;
        }

        // open/close button
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ScaleY(4.0f));

        ImGui::SetNextItemWidth(fullWidth - ScaleX(220.0f));
        ImGui::ColorEdit4("Button", (float*)&g_customButtonOpenCloseCol, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_AlphaPreview);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Open/Closed button scalers ─────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        ImGui::Text("OPEN/CLOSED BUTTON");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        extern float g_fDragBgScale;

        ImGui::Text("Scale");
        ImGui::SameLine(ScaleX(380.0f));
        ImGui::SetNextItemWidth(fullWidth - ScaleX(380.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ScaleX(8.0f), style.FramePadding.y));
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize,  ScaleX(60.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, ScaleX(30.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,          style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,       style.Colors[ImGuiCol_SliderGrab]);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, style.Colors[ImGuiCol_SliderGrab]);
        ImGui::SliderFloat("##ToggleScale", &g_fDragBgScale, 1.0f, 20.0f, "%.0f");
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);

        // ── No Background checkbox ──────────────────────────────
        static bool s_btnNoBgInit = false;
        static bool g_btnNoBg = false;
        if (!s_btnNoBgInit) {
            g_btnNoBg     = g_bBtnNoBg;
            s_btnNoBgInit = true;
        }
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  style.Colors[ImGuiCol_FrameBg]);
        ImGui::Checkbox("No Background", &g_btnNoBg);
        ImGui::PopStyleColor(2);
        extern bool g_bBtnNoBg;
        g_bBtnNoBg = g_btnNoBg;
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Visual effects ──────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        ImGui::Text("EFFECTS");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        extern bool g_bSnowEffect;
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, style.Colors[ImGuiCol_FrameBg]);
        ImGui::Checkbox("Snow Fall Effect", &g_bSnowEffect);
        ImGui::PopStyleColor(2);
        //
                //
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Lock window & Toggle button features ───────────────
        // Initialize from globals (which are set by LoadConfig) on first use.
        static bool s_lockWindowInit = false;
        static bool g_lockWindow = false;
        static bool g_lockOpenCloseButton = false;
        static bool g_lockKeyboard = false; // <-- 1. Add static tracking variable
        if (!s_lockWindowInit) {
            g_lockWindow          = g_bLockWindow;
            g_lockOpenCloseButton = g_bLockOpenCloseBtn;
            g_lockKeyboard        = g_bLockKeyboard; // <-- 2. Initialize from global config
            s_lockWindowInit = true;
        }
        
        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        ImGui::Text("WINDOW");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, style.Colors[ImGuiCol_FrameBg]);
        ImGui::Checkbox("Lock Window (No Move/Resize)", &g_lockWindow);
        ImGui::Spacing();
        ImGui::Checkbox("Lock Open/Close button (No Move)", &g_lockOpenCloseButton);
        ImGui::Spacing();
        
        // 3. Render the new keyboard layout checkbox directly below
        ImGui::Checkbox("Lock Keyboard (No Move/Resize)", &g_lockKeyboard); 
        ImGui::PopStyleColor(2);

        extern bool g_bLockWindow;
        g_bLockWindow = g_lockWindow;
        
        extern bool g_bLockOpenCloseBtn;
        g_bLockOpenCloseBtn = g_lockOpenCloseButton;

        // 4. Update the external global so your SaveConfig and keyboard logic see the change
        extern bool g_bLockKeyboard;
        g_bLockKeyboard = g_lockKeyboard;
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Save Config ────────────────────────────────────────
        static bool g_saveConfirm = false;
        float touchAlphaSave = style.Colors[ImGuiCol_WindowBg].w;
        ImVec4 saveActiveCol = ImVec4(style.Colors[ImGuiCol_CheckMark].x, style.Colors[ImGuiCol_CheckMark].y, style.Colors[ImGuiCol_CheckMark].z, touchAlphaSave);
        ImGui::PushStyleColor(ImGuiCol_Button,        style.Colors[ImGuiCol_Button]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, saveActiveCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  saveActiveCol);
        // default 64.0f
        if (ImGui::Button("Save Config", ImVec2(fullWidth, ScaleY(56.0f)))) {
            SaveConfig();
            g_saveConfirm = true;
        }
        ImGui::PopStyleColor(3);
        if (g_saveConfirm) {
            ImGui::PushStyleColor(ImGuiCol_Text, dimText);
            ImGui::Text("Saved!");
            ImGui::PopStyleColor();
            // Clear the message after ~2 seconds
            static float g_saveConfirmTimer = 0.0f;
            g_saveConfirmTimer += ImGui::GetIO().DeltaTime;
            if (g_saveConfirmTimer > 2.0f) { g_saveConfirm = false; g_saveConfirmTimer = 0.0f; }
        }

    } 

    auto end = imgui.m_pMenuRenderListeners.end();
    for (auto it = imgui.m_pMenuRenderListeners.begin(); it != end; ++it) {
        if (*it) { ((void(*)())(*it))(); pImGui->Separator(); }
    }

    ImGui::End();
}

// ── InitRenderware hook ──────────────────────────────────────
DECL_HOOK(bool, InitRenderware)
{
    if(!InitRenderware()) return false;
    InitRenderWareFunctions();

    nDisplayX = RsGlobal->maximumWidth;
    nDisplayY = RsGlobal->maximumHeight;
    flScaleX  = nDisplayY * 0.00052083333f;
    flScaleY  = nDisplayY * 0.00092592592f;
    displaySize.x = nDisplayX;
    displaySize.y = nDisplayY;
    bImGuiInitialized = true;

    imguiCtx = ImGui::CreateContext();
    ImGui_ImplRenderWare_Init();
    ImGuiIO& io = ImGui::GetIO();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScrollbarSize    = ScaleY(55.0f);
    style.WindowBorderSize = 1.0f;
    style.WindowRounding   = 14.0f;
    style.FrameRounding    = 7.0f;
    style.PopupRounding    = 10.0f;
    style.ItemSpacing      = ImVec2(ScaleX(10.0f), ScaleY(8.0f));

    style.Colors[ImGuiCol_WindowBg]         = ImVec4(0.05f, 0.07f, 0.15f, 0.97f);
    style.Colors[ImGuiCol_TitleBg]          = ImVec4(0.08f,0.14f,0.32f,1.0f);
    style.Colors[ImGuiCol_TitleBgActive]    = ImVec4(0.08f,0.14f,0.32f,1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(style.Colors[ImGuiCol_TitleBg].x, style.Colors[ImGuiCol_TitleBg].y, style.Colors[ImGuiCol_TitleBg].z, 0.5f);
    style.Colors[ImGuiCol_Border] = ImVec4(style.Colors[ImGuiCol_TitleBg].x, style.Colors[ImGuiCol_TitleBg].y, style.Colors[ImGuiCol_TitleBg].z, 1.0f);
    style.Colors[ImGuiCol_SliderGrab]       = ImVec4(0.25f,0.55f,0.90f,1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.35f,0.65f,1.00f,1.0f);
    style.Colors[ImGuiCol_Button]           = ImVec4(0.12f,0.30f,0.60f,1.0f);
    style.Colors[ImGuiCol_ButtonHovered]    = ImVec4(0.18f,0.40f,0.75f,1.0f);
    style.Colors[ImGuiCol_ButtonActive]     = ImVec4(0.10f,0.25f,0.50f,1.0f);
    style.Colors[ImGuiCol_FrameBg]          = ImVec4(0.08f,0.14f,0.30f,1.0f);
    style.Colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.12f,0.20f,0.40f,1.0f);
    style.Colors[ImGuiCol_FrameBgActive]    = ImVec4(0.08f,0.14f,0.30f,1.0f);
    style.Colors[ImGuiCol_CheckMark]        = ImVec4(0.25f,0.55f,0.90f,1.0f);
    style.Colors[ImGuiCol_Separator]        = ImVec4(0.15f,0.25f,0.50f,0.8f);
    style.Colors[ImGuiCol_ScrollbarBg]      = ImVec4(0.04f,0.06f,0.14f,1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab]    = ImVec4(0.20f,0.40f,0.70f,1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = style.Colors[ImGuiCol_ScrollbarGrab];
    style.Colors[ImGuiCol_ScrollbarGrabActive]  = style.Colors[ImGuiCol_ScrollbarGrab];
    style.Colors[ImGuiCol_Header]           = ImVec4(0.12f,0.30f,0.60f,1.0f);
    style.Colors[ImGuiCol_HeaderHovered]    = ImVec4(0.18f,0.40f,0.75f,1.0f);
    style.Colors[ImGuiCol_HeaderActive]     = ImVec4(0.10f,0.25f,0.50f,1.0f);
    style.Colors[ImGuiCol_Separator]        = ImVec4(0.15f,0.30f,0.55f,0.8f);
    style.Colors[ImGuiCol_ScrollbarBg]      = ImVec4(0.04f,0.07f,0.18f,1.0f);
    style.Colors[ImGuiCol_ResizeGrip]       = ImVec4(0.12f,0.30f,0.60f,0.6f);
    style.Colors[ImGuiCol_ResizeGripHovered]= ImVec4(0.18f,0.40f,0.75f,0.9f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.10f,0.25f,0.50f,1.0f);

    imgui.m_pFont = io.Fonts->AddFontFromMemoryTTF((void*)arialData, sizeof(arialData), ScaleY(34.0f), NULL, ranges);

    ma_engine_config cfg = ma_engine_config_init();
    if (ma_engine_init(&cfg, &g_engine) == MA_SUCCESS) {
        g_engineReady = true;
        logger->Info("Music Player: audio engine ready");
    } else {
        logger->Error("Music Player: failed to init audio engine");
    }

    ScanMusicFolder();
    LoadConfig();
    // Write a default config immediately if none existed yet,
    // so the file is always present and the path is confirmed writable.
    {
        std::ifstream check(CONFIG_PATH);
        if (!check.is_open()) SaveConfig();
    }
    return true;
    
}

// ── ShutdownRenderware hook ──────────────────────────────────
DECL_HOOKv(ShutdownRenderware)
{
    SaveConfig();
    StopAndUnload();
    if (g_engineReady) {
        ma_engine_uninit(&g_engine);
        g_engineReady = false;
    }
    ImGui_ImplRenderWare_ShutDown();
    ImGui::DestroyContext();
    ShutdownRenderware();
}

// ── Render2DStuff hook ───────────────────────────────────────
DECL_HOOKv(Render2DStuff)
{
    Render2DStuff();

    ImGui::SetCurrentContext(imguiCtx); // ← reclaim our context every frame
    ImGui_ImplRenderWare_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();

    static float lastTime = 0.0f;
    float now = (float)ImGui::GetTime();
    float dt  = now - lastTime;
    if (dt > 0.1f) dt = 0.1f;
    lastTime  = now;
        static bool wasPlayingBeforeFocusLost = false;
    bool gamePaused = (m_UserPause) ? (*m_UserPause) : false;

    static bool* ms_bCutsceneRunning = nullptr;
    if (!ms_bCutsceneRunning)
        ms_bCutsceneRunning = (bool*)aml->GetSym(pGameHandle, "_ZN12CCutsceneMgr10ms_runningE");
    bool inCutscene = ms_bCutsceneRunning && *ms_bCutsceneRunning;

    bool isMinimized = io.AppFocusLost || 
                       (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) ||
                       (io.MousePos.x <= -9999.0f && io.MousePos.y <= -9999.0f && !io.MouseDown);

    if (isMinimized) {
        if (g_isPlaying && g_soundLoaded) {
            ma_sound_stop(&g_sound);
            g_isPlaying = false;
            wasPlayingBeforeFocusLost = true; 
        }
        ImGui::EndFrame(); 
        return; 
    }

    if (!isMinimized && wasPlayingBeforeFocusLost && g_soundLoaded) {
        ma_sound_start(&g_sound);
        g_isPlaying = true;
        wasPlayingBeforeFocusLost = false;
    }

    if (gamePaused && g_isPlaying && g_soundLoaded) {
        ma_sound_stop(&g_sound);
        g_isPlaying = false;
    }

    // Apply theme loaded from config on the very first live frame
    if (g_bPendingThemeApply) {
        ApplyThemeToStyle(g_theme);
        g_bPendingThemeApply = false;
    }
    
    UpdateVisualizer(dt);
    CheckTrackEnd();

    if (bDisplaySpecialImGuiMenu && !inCutscene)
        DrawMusicPlayer(&bDisplaySpecialImGuiMenu);

    // Cache window positions every frame so SaveConfig() works without a live context

    extern float g_fDragBgScale;
    float circleRadius = ScaleY(g_fDragBgScale * 4.0f); 
    if (circleRadius < ScaleY(25.0f)) circleRadius = ScaleY(25.0f); 
    
    float windowSize = (circleRadius * 2.0f) + ScaleX(30.0f); 

    if (!inCutscene)
    {
        ImGui::SetNextWindowBgAlpha(0.0f); 
        {
            static bool s_togglePosSet = false;
            if (!s_togglePosSet) {
                ImVec2 togglePos = (g_config.btnPosX >= 0.0f && g_config.btnPosY >= 0.0f)
                    ? ImVec2(g_config.btnPosX, g_config.btnPosY)
                    : ImVec2(displaySize.x - ScaleX(250.0f), displaySize.y - ScaleY(250.0f));
                ImGui::SetNextWindowPos(togglePos, ImGuiCond_Always);
                s_togglePosSet = true;
            }
        }
        ImGui::SetNextWindowSize({windowSize, windowSize}, ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    extern bool g_bLockOpenCloseBtn;
    ImGuiWindowFlags toggleBtnFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus;
    
    if (g_bLockOpenCloseBtn) {
        toggleBtnFlags |= ImGuiWindowFlags_NoMove;
    }

    // Always track position regardless of cutscene state
    if (ImGuiWindow* w = ImGui::FindWindowByName("MusicPlayerToggle")) {
        g_config.btnPosX = w->Pos.x;
        g_config.btnPosY = w->Pos.y;
    }

    if (!inCutscene && ImGui::Begin("MusicPlayerToggle", NULL, toggleBtnFlags))
    {
        
        auto CheckboxWindow = ImGui::GetCurrentWindow();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 screenPos = ImGui::GetCursorScreenPos();
        
        ImVec2 center = ImVec2(screenPos.x + circleRadius + ScaleX(10.0f), screenPos.y + circleRadius + ScaleY(10.0f));

        // open/close button
        extern ImVec4 g_customButtonOpenCloseCol;
        ImU32 btnAlpha = (ImU32)(g_customButtonOpenCloseCol.w * 255.0f);
        ImU32 colorInnerBg = IM_COL32(40, 40, 40, (ImU32)(70 * g_customButtonOpenCloseCol.w));
        ImU32 colorRing    = IM_COL32(0, 0, 0, (ImU32)(180 * g_customButtonOpenCloseCol.w));
        ImU32 colorText    = IM_COL32((ImU32)(g_customButtonOpenCloseCol.x * 255), (ImU32)(g_customButtonOpenCloseCol.y * 255), (ImU32)(g_customButtonOpenCloseCol.z * 255), btnAlpha);
        ImU32 colorShadow  = IM_COL32(0, 0, 0, (ImU32)(80 * g_customButtonOpenCloseCol.w));
        
        ImVec2 shadowOffset = ImVec2(ScaleX(2.5f), ScaleY(2.5f));
        ImVec2 shadowCenter = ImVec2(center.x + shadowOffset.x, center.y + shadowOffset.y);
        
        extern bool g_bBtnNoBg;
        if (!g_bBtnNoBg) {
            drawList->AddCircleFilled(shadowCenter, circleRadius + 1.0f, colorShadow, 64);
            drawList->AddCircle(shadowCenter, circleRadius + 1.5f, IM_COL32(0, 0, 0, (ImU32)(45 * g_customButtonOpenCloseCol.w)), 64, 2.0f);
            drawList->AddCircleFilled(center, circleRadius, colorInnerBg, 64);
        }
                
        float ringThickness = 5.0f; 
        drawList->AddCircle(center, circleRadius, colorRing, 64, ringThickness);

        float fontScale = (g_fDragBgScale * 0.4f + 5.6f) / 14.0f * 2.5f;
        ImGui::SetWindowFontScale(fontScale);

        const char* iconText = "M";
        ImVec2 textSize = ImGui::CalcTextSize(iconText);
        ImVec2 textPos = ImVec2(center.x - (textSize.x * 0.5f), center.y - (textSize.y * 0.5f));
        drawList->AddText(textPos, colorText, iconText);

        ImGui::SetWindowFontScale(1.0f);

        ImGui::SetCursorScreenPos(ImVec2(center.x - circleRadius, center.y - circleRadius));
        
        ImGui::InvisibleButton("##CircleHitbox", ImVec2(circleRadius * 2.0f, circleRadius * 2.0f));
        
        if (!g_bLockOpenCloseBtn && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            ImVec2 windowPos = ImGui::GetWindowPos();
            ImGui::SetWindowPos("MusicPlayerToggle", ImVec2(windowPos.x + delta.x, windowPos.y + delta.y));
        }
        
        if (ImGui::IsItemDeactivatedAfterEdit() || (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)))
        {
            bDisplaySpecialImGuiMenu = !bDisplaySpecialImGuiMenu;
        }

        if (!g_bLockOpenCloseBtn) {
            drawList->AddText(ImVec2(center.x - (ImGui::CalcTextSize("(Drag)").x * 0.5f), center.y + circleRadius + 5.0f), IM_COL32(0, 0, 0, (ImU32)(180 * g_customButtonOpenCloseCol.w)), "(Drag)");
        }
        
        ImGui::BringWindowToDisplayFront(CheckboxWindow);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    }
    TrackWindowPositions();

    if (GTA_RequestKeyboard) GTA_RequestKeyboard(false);
    
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplRenderWare_RenderDrawData(ImGui::GetDrawData());

    if (nClearMousePos > 0) {
        if (--nClearMousePos == 0)
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
}

static uint8_t fingers      = 0;
static uint8_t fingerAsMouse = 0xFF;
static bool    g_bIgnoredFingers[10] = {false};

inline bool CanProcessImTouch()
{
    if (!bImGuiInitialized) return false;
    bool gamePaused = (m_UserPause)   ? (*m_UserPause)        : false;
    if (gamePaused) return false;
    return true;
}

static bool IsPointOverImGuiUI(float x, float y)
{
    extern bool bDisplaySpecialImGuiMenu;

    if (ImGuiWindow* w = ImGui::FindWindowByName("MusicPlayerToggle")) {
        if (w->Active) {
            ImVec2 p = w->Pos, s = w->Size;
            if (x >= p.x && x <= p.x + s.x && y >= p.y && y <= p.y + s.y)
                return true;
        }
    }

    if (bDisplaySpecialImGuiMenu) {
        if (ImGuiWindow* w = ImGui::FindWindowByName("Music Player")) {
            if (w->Active) {
                ImVec2 p = w->Pos, s = w->Size;
                if (x >= p.x && x <= p.x + s.x && y >= p.y && y <= p.y + s.y)
                    return true;
            }
        }

        if (ImGuiWindow* w = ImGui::FindWindowByName("Keyboard")) {
            if (w->Active) {
                ImVec2 p = w->Pos, s = w->Size;
                if (x >= p.x && x <= p.x + s.x && y >= p.y && y <= p.y + s.y)
                    return true;
            }
        }
    }
    return false;
}

DECL_HOOKv(OnTouchEvent, int type, int fingerId, int x, int y)
{
    ImGui::SetCurrentContext(imguiCtx); // ← reclaim our context every touch event
    ImGuiIO* io = &ImGui::GetIO();

    if (type == TOUCH_PUSH)   ++fingers;
    else if (type == TOUCH_RELEASE) --fingers;

    if (fingers == 2 && x < 150 && y < 150)
        bDisplaySpecialImGuiMenu = true;

    if (!CanProcessImTouch()) {
        OnTouchEvent(type, fingerId, x, y);
        return;
    }

    switch (type) {
    case TOUCH_PUSH:
    {
        bool overPopup = false;
        if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            ImGuiContext& g = *GImGui;
            for (int i = 0; i < g.Windows.Size; i++) {
                ImGuiWindow* w = g.Windows[i];
                if (w->Active && (w->Flags & ImGuiWindowFlags_Popup)) {
                    if ((float)x >= w->Pos.x && (float)x <= w->Pos.x + w->Size.x &&
                        (float)y >= w->Pos.y && (float)y <= w->Pos.y + w->Size.y) {
                        overPopup = true;
                        break;
                    }
                }
            }
        }
        if (IsPointOverImGuiUI((float)x, (float)y) || overPopup) {
            io->AddMousePosEvent(x, y);
            io->AddMouseButtonEvent(0, true);
            fingerAsMouse = fingerId;
            g_bIgnoredFingers[fingerId] = true;
        } else {
            g_bIgnoredFingers[fingerId] = false;
            OnTouchEvent(type, fingerId, x, y);
        }
        break;
    }

    case TOUCH_RELEASE:
        if (fingerAsMouse == fingerId) {
            io->AddMousePosEvent(x, y);
            io->AddMouseButtonEvent(0, false);
            nClearMousePos = FRAMES_TO_CLEAR_MOUSE;
            fingerAsMouse  = 0xFF;
        }
        if (!g_bIgnoredFingers[fingerId])
            OnTouchEvent(type, fingerId, x, y);
        g_bIgnoredFingers[fingerId] = false;
        break;

    case TOUCH_MOVE:
        if (fingerAsMouse == fingerId) {
            io->AddMousePosEvent(x, y);
        }
        if (!g_bIgnoredFingers[fingerId])
            OnTouchEvent(type, fingerId, x, y);
        break;

    default:
        OnTouchEvent(type, fingerId, x, y);
        break;
    }
}

DECL_HOOKv(GTA_KeyboardEvent, bool pushed, int keyNum, int ctrl_or_shift, int alwaysZero)
{
    GTA_KeyboardEvent(pushed, keyNum, ctrl_or_shift, alwaysZero);
}

extern "C" void OnModPreLoad()
{
    logger->SetTag("Music Player ImGui");
    pGameLib = aml->GetLib("libGTASA.so");
    if (pGameLib) {
        pGameHandle = aml->GetLibHandle("libGTASA.so");
    } else {
        logger->Error("Cannot determine GTA SA library handle!");
        return;
    }

    SET_TO(nearScreenZ,         aml->GetSym(pGameHandle, "_ZN9CSprite2d11NearScreenZE"));
    SET_TO(recipNearClip,       aml->GetSym(pGameHandle, "_ZN9CSprite2d13RecipNearClipE"));
    SET_TO(SetScissorRect,      aml->GetSym(pGameHandle, "_ZN7CWidget10SetScissorER5CRect"));
    SET_TO(GetScreenFadeStatus, aml->GetSym(pGameHandle, "_ZN7CCamera19GetScreenFadeStatusEv"));
    SET_TO(pTheCamera,          aml->GetSym(pGameHandle, "TheCamera"));

    RegisterInterface("ImGui", pImGui);
}

extern "C" void OnModLoad()
{
    if (!pGameLib) return;

    HOOK(InitRenderware,    aml->GetSym(pGameHandle, "_ZN5CGame20InitialiseRenderWareEv"));
    HOOK(OnTouchEvent,      aml->GetSym(pGameHandle, "_Z14AND_TouchEventiiii"));
    HOOK(GTA_KeyboardEvent, aml->GetSym(pGameHandle, "_Z17AND_KeyboardEventbiib"));
    HOOK(Render2DStuff,     aml->GetSym(pGameHandle, "_Z13Render2dStuffv"));

    SET_TO(m_UserPause,     aml->GetSym(pGameHandle, "_ZN6CTimer11m_UserPauseE"));
    SET_TO(GTA_RequestKeyboard, aml->GetSym(pGameHandle, "_Z18OS_KeyboardRequesti"));
}

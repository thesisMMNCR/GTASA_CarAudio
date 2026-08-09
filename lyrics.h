#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <sys/stat.h> // Required for checking and making folders via mkdir()

struct LyricLine {
    float timeInSeconds;
    std::string text;
};

// Global states exposed safely to main.cpp
inline std::vector<LyricLine> g_currentLyrics;
inline std::string g_currentLyricsLineText = "";

// Helper to clean up timestamps like "[01:23.45]" into raw seconds
inline float ParseLrcTime(const std::string& timeStr) {
    int mins = 0, secs = 0;
    float ms = 0.0f;
    if (sscanf(timeStr.c_str(), "[%d:%d.%f]", &mins, &secs, &ms) >= 2) {
        return (mins * 60.0f) + secs + (ms / 100.0f);
    }
    return 0.0f;
}

// Scans and parses the .lrc file paired with the current song inside the /lyrics/ folder
inline void LoadLyricsForSong(const std::string& songPath) {
    g_currentLyrics.clear();
    g_currentLyricsLineText = "";

    // Separate the folder directory path and the raw file name
    size_t lastSlash = songPath.find_last_of("/");
    if (lastSlash == std::string::npos) return;

    std::string baseDir  = songPath.substr(0, lastSlash);   // e.g., /.../files/music
    std::string fileName = songPath.substr(lastSlash + 1); // e.g., track.mp3

    // ── AUTO-FOLDER CREATION CHECK ──
    // Checks if the /lyrics folder exists; if not, it builds it automatically.
    std::string lyricsDir = baseDir + "/lyrics";
    struct stat st;
    if (stat(lyricsDir.c_str(), &st) != 0) {
        // Folder does not exist, let's create it with proper read/write permissions
        mkdir(lyricsDir.c_str(), 0777);
        logger->Info("MusicPlayer: Created missing lyrics folder at: %s", lyricsDir.c_str());
    }

    // Find the file extension dot to drop it (.mp3, .wav, etc.)
    size_t lastDot = fileName.find_last_of(".");
    if (lastDot == std::string::npos) return;
    std::string songNameOnly = fileName.substr(0, lastDot); // e.g., track

    // Target the full .lrc path structure inside the newly verified folder
    std::string lrcPath = lyricsDir + "/" + songNameOnly + ".lrc";

    logger->Info("MusicPlayer: Scanning for lyrics file at: %s", lrcPath.c_str());

    FILE* file = fopen(lrcPath.c_str(), "r");
    if (!file) {
        logger->Error("MusicPlayer: No lyric file found at path! Place a matching file here to use lyrics.");
        return; 
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        std::string lineStr(line);
        // Only parse lines that start with timestamp brackets e.g. [00:12.34]
        if (lineStr.length() > 10 && lineStr[0] == '[') {
            size_t closeBracket = lineStr.find(']');
            if (closeBracket != std::string::npos) {
                std::string timePart = lineStr.substr(0, closeBracket + 1);
                std::string textPart = lineStr.substr(closeBracket + 1);
                
                // Strip out trailing carriage returns and newlines
                textPart.erase(std::remove(textPart.begin(), textPart.end(), '\n'), textPart.end());
                textPart.erase(std::remove(textPart.begin(), textPart.end(), '\r'), textPart.end());

                LyricLine l;
                l.timeInSeconds = ParseLrcTime(timePart);
                l.text = textPart;
                g_currentLyrics.push_back(l);
            }
        }
    }
    fclose(file);

    // Keep timestamps properly arranged sequentially for the real-time sync loop
    std::sort(g_currentLyrics.begin(), g_currentLyrics.end(), 
        [](const LyricLine& a, const LyricLine& b) {
            return a.timeInSeconds < b.timeInSeconds;
        });
        
    logger->Info("MusicPlayer: Successfully loaded %d synchronized lines!", (int)g_currentLyrics.size());
}

// Compares current playback position against timestamps to sync the active line text
inline void UpdateLyricsSync(float currentTimeSeconds) {
    std::string foundLyric = "";
    for (const auto& line : g_currentLyrics) {
        if (currentTimeSeconds >= line.timeInSeconds) {
            foundLyric = line.text;
        } else {
            break; // Array is sorted sequentially, we can safely exit tracking early
        }
    }
    g_currentLyricsLineText = foundLyric;
}


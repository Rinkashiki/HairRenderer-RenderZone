#pragma once

#include <filesystem>
#include <string>

class VideoEncoder
{
  public:
    // Encode the PNG sequence in framesDir into outputPath.
    // framesDir must contain files named frame_00000.png, frame_00001.png, ...
    // ffmpegExe: path to the ffmpeg binary (default "ffmpeg" = use system PATH).
    // Throws std::runtime_error if ffmpeg is not found or exits non-zero.
    // On failure the caller should retain framesDir for diagnosis.
    static void encode(const std::filesystem::path& framesDir,
                       const std::string&           outputPath,
                       float                        fps,
                       const std::string&           ffmpegExe = "ffmpeg");
};

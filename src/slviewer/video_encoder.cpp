#include "video_encoder.h"

#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>

void VideoEncoder::encode(const std::filesystem::path& framesDir,
                          const std::string&           outputPath,
                          float                        fps,
                          const std::string&           ffmpegExe)
{
    int fpsi = static_cast<int>(std::round(fps));
    if (fpsi <= 0)
        fpsi = 30;

    const std::string inputPattern = (framesDir / "frame_%05d.png").string();

#ifdef _WIN32
    std::string cmd = "\"" + ffmpegExe + "\" -y -framerate " + std::to_string(fpsi) +
                      " -i \"" + inputPattern + "\"" +
                      " -c:v libx264 -pix_fmt yuv420p -crf 18" +
                      " \"" + outputPath + "\"";
    // std::system() runs `cmd.exe /c <cmd>`. When the string starts with a quote
    // and contains several quoted tokens, cmd.exe strips the outermost quote pair
    // and corrupts the path (see `cmd /?`). Wrapping the whole line in one extra
    // pair of quotes makes the inner quoting survive intact.
    cmd = "\"" + cmd + "\"";
#else
    std::string cmd = "'" + ffmpegExe + "' -y -framerate " + std::to_string(fpsi) +
                      " -i '" + inputPattern + "'" +
                      " -c:v libx264 -pix_fmt yuv420p -crf 18" +
                      " '" + outputPath + "'";
#endif

    int ret = std::system(cmd.c_str());
    if (ret != 0)
        throw std::runtime_error("ffmpeg exited with code " + std::to_string(ret) +
                                 " — see ffmpeg output above for details");
}

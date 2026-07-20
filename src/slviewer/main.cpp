#include <iostream>
#include <string>

#include "application_sl.h"
#include "resource_paths.h"

int main(int argc, char* argv[]) {
    if (argc < 2)
    {
        std::cerr << "Usage: SLViewer <animation.json> [--scene scene.json] [--output out.mp4] [--width W] [--height H] [--msaa 1|4|8] [--keep-frames] [--log-level error|warn|verbose]\n";
        return EXIT_FAILURE;
    }

    std::string animPath    = argv[1];
    std::string scenePath;  // empty → use bundled default (scenes/maria.json)
    std::string outputPath  = "output.mp4";
    int         width       = 1920;
    int         height      = 1080;
    bool        keepFrames  = false;
    LogLevel    logLevel    = LogLevel::Warn;
    MSAASamples msaa        = MSAASamples::x8;

    for (int i = 2; i < argc; ++i)
    {
        std::string token(argv[i]);
        if (token == "--scene" && i + 1 < argc)
        {
            scenePath = argv[++i];
        }
        else if (token == "--output" && i + 1 < argc)
        {
            outputPath = argv[++i];
        }
        else if (token == "--width" && i + 1 < argc)
        {
            width = std::stoi(argv[++i]);
        }
        else if (token == "--height" && i + 1 < argc)
        {
            height = std::stoi(argv[++i]);
        }
        else if (token == "--msaa" && i + 1 < argc)
        {
            std::string m(argv[++i]);
            if (m == "1")      msaa = MSAASamples::x1;
            else if (m == "4") msaa = MSAASamples::x4;
            else if (m == "8") msaa = MSAASamples::x8;
            else {
                std::cerr << "--msaa expects 1, 4, or 8 (got \"" << m << "\")\n";
                return EXIT_FAILURE;
            }
        }
        else if (token == "--keep-frames")
        {
            keepFrames = true;
        }
        else if (token == "--log-level" && i + 1 < argc)
        {
            std::string level(argv[++i]);
            if (level == "error")
                logLevel = LogLevel::Error;
            else if (level == "verbose")
                logLevel = LogLevel::Info;
            // default: warn
        }
        else
        {
            std::cerr << "Unknown argument: " << token << "\n";
            return EXIT_FAILURE;
        }
    }

    std::string resourcesPath;
    try {
        resourcesPath = discover_resources_path();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    try {
        SLApplication app;
        app.run(animPath, scenePath, outputPath, resourcesPath, width, height, keepFrames, logLevel, msaa);
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

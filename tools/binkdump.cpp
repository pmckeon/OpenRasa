// binkdump: decodes a Bink movie with OpenRasa's decoder (src/media/Bink) - the pictures as raw yuv420p (cropped to the movie's size,
// as ffmpeg writes them) and the audio as interleaved f32le - for comparison with ffmpeg's decode.
// binkdump <movie.bik> <out.yuv> <out.f32> [frames]
#include "media/Bink.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "binkdump <movie.bik> <out.yuv> <out.f32> [frames]\n");
        return 2;
    }
    media::BinkMovie m;
    std::string error;
    if (!m.open(argv[1], &error)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
        return 1;
    }
    std::printf("%d x %d, %u frames, %.3f fps, audio %d Hz x %d\n", m.width(), m.height(), m.frameCount(), m.framesPerSecond(),
                m.audioRate(), m.audioChannels());
    FILE* yuv = std::fopen(argv[2], "wb");
    FILE* pcm = std::fopen(argv[3], "wb");
    const uint32_t limit = argc > 4 ? uint32_t(std::atoi(argv[4])) : m.frameCount();
    const auto start = std::chrono::steady_clock::now();
    size_t samples = 0;
    for (uint32_t f = 0; f < limit && f < m.frameCount(); ++f) {
        if (!m.decodeNext(&error)) {
            std::fprintf(stderr, "frame %u: %s\n", f, error.c_str());
            return 1;
        }
        for (int p = 0; p < 3; ++p) {
            const int w = p ? m.width() / 2 : m.width(), h = p ? m.height() / 2 : m.height();
            for (int y = 0; y < h; ++y) std::fwrite(m.plane(p) + y * m.stride(p), 1, size_t(w), yuv);
        }
        const std::vector<float> a = m.takeAudio();
        samples += a.size();
        std::fwrite(a.data(), sizeof(float), a.size(), pcm);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("decoded in %.0f ms (%.2f ms a frame), %zu audio samples\n", ms, ms / limit, samples);
    std::fclose(yuv);
    std::fclose(pcm);
    return 0;
}

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Fixture {
    std::string path;
    std::string rate;
    int expected_frames = 1;
    std::uintmax_t max_bytes = 0;
};

std::string shellQuote(const std::string& value) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::map<std::string, int> parseMetrics(const std::string& text) {
    std::map<std::string, int> metrics;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string key;
        int value = 0;
        if (row >> key >> value) {
            metrics[key] = value;
        }
    }
    return metrics;
}

bool expectMetric(const Fixture& fixture,
                  const std::map<std::string, int>& metrics,
                  const std::string& key,
                  int expected) {
    const auto it = metrics.find(key);
    if (it == metrics.end()) {
        std::cerr << fixture.path << ": missing metric " << key << "\n";
        return false;
    }
    if (it->second != expected) {
        std::cerr << fixture.path << ": " << key << "=" << it->second
                  << ", expected " << expected << "\n";
        return false;
    }
    return true;
}

bool runFixture(const std::filesystem::path& decode_bench,
                const std::filesystem::path& source_dir,
                const Fixture& fixture) {
    const std::filesystem::path wav_path = source_dir / fixture.path;
    if (!std::filesystem::exists(wav_path)) {
        std::cerr << fixture.path << ": fixture missing\n";
        return false;
    }
    if (fixture.max_bytes > 0 && std::filesystem::file_size(wav_path) > fixture.max_bytes) {
        std::cerr << fixture.path << ": fixture is "
                  << std::filesystem::file_size(wav_path)
                  << " bytes, expected <= " << fixture.max_bytes << "\n";
        return false;
    }

    const std::filesystem::path log_path =
        std::filesystem::temp_directory_path() /
        ("projectultra_decode_bench_replay_" + wav_path.stem().string() + ".log");
    std::ostringstream cmd;
    cmd << shellQuote(decode_bench.string())
        << " --mode bench --connected"
        << " --wav " << shellQuote(wav_path.string())
        << " --waveform ofdm_chirp --rate " << fixture.rate
        << " --mod dqpsk --cw-count 4"
        << " > " << shellQuote(log_path.string()) << " 2>&1";

    const int rc = std::system(cmd.str().c_str());
    const std::string output = readFile(log_path);
    if (rc != 0) {
        std::cerr << fixture.path << ": decode_bench exited " << rc << "\n"
                  << output << "\n";
        return false;
    }

    const auto metrics = parseMetrics(output);
    bool ok = true;
    ok &= expectMetric(fixture, metrics, "frames_decoded", fixture.expected_frames);
    ok &= expectMetric(fixture, metrics, "frames_failed", 0);
    ok &= expectMetric(fixture, metrics, "data_frames_decoded", fixture.expected_frames);
    ok &= expectMetric(fixture, metrics, "byte_exact_ok", fixture.expected_frames);
    ok &= expectMetric(fixture, metrics, "byte_exact_bad", 0);
    if (ok) {
        std::cout << fixture.path << " ok: "
                  << fixture.expected_frames << " byte-exact DATA frame(s)\n";
    } else {
        std::cerr << output << "\n";
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <decode_bench> <source_dir>\n";
        return 2;
    }

    constexpr std::uintmax_t kSmallFixtureMaxBytes = 200 * 1024;
    const std::vector<Fixture> fixtures = {
        {"fixtures/ofdm_chirp_r14_dqpsk_clean.wav", "r1_4", 4, 0},
        {"fixtures/ofdm_chirp_r14_dqpsk_snr15_awgn.wav", "r1_4", 4, 0},
        {"fixtures/ofdm_chirp_r12_dqpsk_snr15_awgn.wav", "r1_2", 1, kSmallFixtureMaxBytes},
        {"fixtures/ofdm_chirp_r34_dqpsk_snr15_awgn.wav", "r3_4", 1, kSmallFixtureMaxBytes},
        {"fixtures/ofdm_chirp_r14_dqpsk_snr15_good.wav", "r1_4", 1, kSmallFixtureMaxBytes},
        {"fixtures/ofdm_chirp_r12_dqpsk_snr15_good.wav", "r1_2", 1, kSmallFixtureMaxBytes},
    };

    bool ok = true;
    for (const auto& fixture : fixtures) {
        ok &= runFixture(argv[1], argv[2], fixture);
    }
    return ok ? 0 : 1;
}

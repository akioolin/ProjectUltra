#include "protocol/file_transfer.hpp"
#include "helpers/temp_dir.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace ultra;
using namespace ultra::protocol;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
            return; \
        } \
    } while (0)

struct TxChunk {
    Bytes payload;
    bool more_data = false;
};

Bytes makeCompressiblePayload() {
    Bytes data;
    const std::string phrase =
        "ProjectUltra HF modem file-transfer compressed reorder regression.\n";
    for (int i = 0; i < 512; ++i) {
        data.insert(data.end(), phrase.begin(), phrase.end());
        data.push_back(static_cast<uint8_t>('A' + (i % 4)));
    }
    return data;
}

bool writeFile(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out.good()) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    return out.good();
}

Bytes readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.good()) {
        return {};
    }

    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);

    Bytes data(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

std::vector<TxChunk> collectChunks(FileTransferController& tx) {
    std::vector<TxChunk> chunks;
    while (tx.hasMoreChunks()) {
        Bytes payload = tx.getNextChunk();
        if (!payload.empty()) {
            chunks.push_back({std::move(payload), tx.hasMoreChunks()});
        }
    }
    return chunks;
}

struct TransferDirs {
    ultra::test::TempDir root;
    std::filesystem::path tx_dir;
    std::filesystem::path rx_dir;
    bool ready = false;

    explicit TransferDirs(const std::string& prefix, const std::string& rx_name = "rx")
        : root(prefix),
          tx_dir(root.child("tx")),
          rx_dir(root.child(rx_name)) {
        if (!root.valid()) {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(tx_dir, ec);
        if (ec) {
            return;
        }
        std::filesystem::create_directories(rx_dir, ec);
        ready = !ec;
    }
};

void test_compressed_final_chunk_out_of_order_finalizes() {
    TransferDirs dirs("ultra_file_transfer_controller_test");
    CHECK(dirs.ready, "create temp directories");

    const Bytes original = makeCompressiblePayload();
    const std::filesystem::path src_path = dirs.tx_dir / "compressed_reorder.txt";
    CHECK(writeFile(src_path, original), "write source file");

    FileTransferController tx;
    tx.setMaxChunkPayload(37);  // 32 compressed data bytes per FILE_DATA chunk.
    CHECK(tx.startSend(src_path.string()), "start compressed send");

    std::vector<TxChunk> chunks = collectChunks(tx);
    CHECK(chunks.size() >= 4, "compressed transfer should span metadata plus multiple data chunks");
    CHECK(chunks.front().payload[0] == static_cast<uint8_t>(PayloadType::FILE_START),
          "first chunk is metadata");
    CHECK(chunks.back().payload[0] == static_cast<uint8_t>(PayloadType::FILE_DATA),
          "last chunk is file data");
    CHECK(!chunks.back().more_data, "last chunk carries final marker");

    FileTransferController rx;
    rx.setReceiveDirectory(dirs.rx_dir.string());

    bool callback_called = false;
    bool callback_success = false;
    std::string received_path;
    rx.setReceivedCallback([&](const std::string& path, bool success) {
        callback_called = true;
        callback_success = success;
        received_path = path;
    });

    CHECK(rx.processPayload(chunks.front().payload, chunks.front().more_data),
          "metadata accepted");

    const size_t last = chunks.size() - 1;
    CHECK(rx.processPayload(chunks[last].payload, chunks[last].more_data),
          "out-of-order final chunk accepted");
    CHECK(!callback_called, "out-of-order final chunk should not finalize before gaps close");

    for (size_t i = 1; i < last; ++i) {
        CHECK(rx.processPayload(chunks[i].payload, chunks[i].more_data),
              "gap-filling chunk accepted");
    }

    CHECK(callback_called, "receiver callback fired after final buffered chunk drained");
    CHECK(callback_success, "receiver reported success");
    CHECK(readFile(received_path) == original, "received file matches original payload");
}

void test_duplicate_filename_in_dotted_receive_directory() {
    TransferDirs dirs("ultra_file_transfer_duplicate_test", "rx.with.dot");
    CHECK(dirs.ready, "create temp directories");

    const Bytes original = {'P', 'r', 'o', 'j', 'e', 'c', 't',
                            'U', 'l', 't', 'r', 'a'};
    const std::filesystem::path src_path = dirs.tx_dir / "payload";
    CHECK(writeFile(src_path, original), "write duplicate source file");
    CHECK(writeFile(dirs.rx_dir / "payload", Bytes{'o', 'l', 'd'}),
          "write existing receive file");

    FileTransferController tx;
    CHECK(tx.startSend(src_path.string()), "start duplicate-name send");
    std::vector<TxChunk> chunks = collectChunks(tx);
    CHECK(chunks.size() >= 2, "duplicate-name transfer has metadata and data");

    FileTransferController rx;
    rx.setReceiveDirectory(dirs.rx_dir.string());

    bool callback_called = false;
    bool callback_success = false;
    std::string received_path;
    rx.setReceivedCallback([&](const std::string& path, bool success) {
        callback_called = true;
        callback_success = success;
        received_path = path;
    });

    for (const auto& chunk : chunks) {
        CHECK(rx.processPayload(chunk.payload, chunk.more_data),
              "duplicate-name chunk accepted");
    }

    const std::filesystem::path received(received_path);
    CHECK(callback_called, "duplicate-name receiver callback fired");
    CHECK(callback_success, "duplicate-name transfer succeeded");
    CHECK(received.parent_path() == dirs.rx_dir,
          "duplicate-name uniquing stays inside dotted receive directory");
    CHECK(received.filename() == "payload_1",
          "duplicate-name uniquing appends suffix to filename without extension");
    CHECK(readFile(received) == original, "duplicate-name received content matches");
    CHECK(readFile(dirs.rx_dir / "payload") == Bytes({'o', 'l', 'd'}),
          "duplicate-name original file preserved");
}

void test_single_block_payload_round_trip() {
    TransferDirs dirs("ultra_file_transfer_block_test");
    CHECK(dirs.ready, "create temp directories");

    const Bytes original = {'b', 'l', 'o', 'c', 'k', 0, 1, 2, 3, 4};
    const std::filesystem::path src_path = dirs.tx_dir / "block.bin";
    CHECK(writeFile(src_path, original), "write block source file");

    FileTransferController tx;
    CHECK(tx.startSend(src_path.string()), "start block send");
    Bytes block = tx.getSingleBlockPayload(1024);
    CHECK(!block.empty(), "single block payload fits");
    CHECK(block[0] == static_cast<uint8_t>(PayloadType::FILE_BLOCK),
          "single block uses FILE_BLOCK type");
    CHECK(!tx.hasMoreChunks(), "single block consumes tx chunks");

    FileTransferController rx;
    rx.setReceiveDirectory(dirs.rx_dir.string());
    bool callback_called = false;
    bool callback_success = false;
    std::string received_path;
    rx.setReceivedCallback([&](const std::string& path, bool success) {
        callback_called = true;
        callback_success = success;
        received_path = path;
    });

    CHECK(rx.processPayload(block, false), "block payload accepted");
    CHECK(callback_called, "block receiver callback fired");
    CHECK(callback_success, "block transfer succeeded");
    CHECK(readFile(received_path) == original, "block received content matches");
}

}  // namespace

int main() {
    test_compressed_final_chunk_out_of_order_finalizes();
    test_duplicate_filename_in_dotted_receive_directory();
    test_single_block_payload_round_trip();

    if (tests_failed != 0) {
        std::cout << "FileTransferController: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "FileTransferController: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}

#include <smg/Archive.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace {
unsigned checks = 0;
void check(bool passed, const char* description) {
    ++checks;
    if (!passed) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}
void put32(std::vector<std::uint8_t>& data, unsigned offset, std::uint32_t value) {
    data[offset]=value>>24; data[offset+1]=value>>16; data[offset+2]=value>>8; data[offset+3]=value;
}
std::vector<std::uint8_t> rarc() {
    // One unaligned three-byte resource and a self-directory entry.
    std::vector<std::uint8_t> data(164, 0);
    std::memcpy(data.data(), "RARC", 4);
    put32(data, 4, data.size()); put32(data, 8, 32); put32(data, 12, 129); put32(data, 16, 3);
    put32(data, 32, 1); put32(data, 36, 32); put32(data, 40, 2); put32(data, 44, 48);
    put32(data, 48, 11); put32(data, 52, 88);
    data[75]=2;
    put32(data, 84, 0x11000000); put32(data, 92, 3);
    put32(data, 104, 0x02000009);
    std::memcpy(data.data()+120, "test.bin\0.\0", 11);
    std::memcpy(data.data()+161, "abc", 3);
    return data;
}
}
int main() {
    std::vector<std::uint8_t> out;
    std::string error;
    std::vector<std::uint8_t> yaz(16, 0);
    std::memcpy(yaz.data(), "Yaz0", 4); put32(yaz, 4, 9);
    yaz.insert(yaz.end(), {0xe0, 'a', 'b', 'c', 0x40, 0x02});
    check(smg::decodeYaz0({yaz.data(),yaz.size()}, out, 9, error), "Yaz0 overlapping back-reference");
    check(std::string(out.begin(),out.end()) == "abcabcabc", "Yaz0 exact decoded contents");
    check(!smg::decodeYaz0({yaz.data(),yaz.size()}, out, 8, error) && out.empty(), "output allocation budget enforced");
    for (std::size_t len=0; len<yaz.size(); ++len)
        check(!smg::decodeYaz0({yaz.data(),len}, out, 9, error), "truncated Yaz0 rejected");
    auto bad=yaz; bad[21]=0xff;
    check(!smg::decodeYaz0({bad.data(),bad.size()}, out, 9, error), "Yaz0 invalid source rejected");
    bad=yaz; bad[20]=0xf0;
    check(!smg::decodeYaz0({bad.data(),bad.size()}, out, 9, error), "Yaz0 output overrun rejected");
    yaz.assign(16, 0); std::memcpy(yaz.data(), "Yaz0", 4); put32(yaz,4,20);
    yaz.insert(yaz.end(), {0x80,'x',0,0,1});
    check(smg::decodeYaz0({yaz.data(),yaz.size()},out,20,error), "Yaz0 extended run length");
    check(out == std::vector<std::uint8_t>(20,'x'), "Yaz0 extended overlapping run contents");

    auto data=rarc(); std::vector<smg::ArchiveEntry> entries;
    check(smg::indexRarc({data.data(),data.size()},entries,error), "RARC big-endian index");
    check(entries.size()==1 && entries[0].path=="test.bin" && entries[0].offset==161 && entries[0].size==3,
        "RARC offset, size and dot-directory handling");
    for (std::size_t len=0; len<data.size(); ++len)
        check(!smg::indexRarc({data.data(),len},entries,error), "truncated RARC rejected");
    bad=data; put32(bad,44,0xfffffff0);
    check(!smg::indexRarc({bad.data(),bad.size()},entries,error), "overflowing RARC offset rejected");
    bad=data; put32(bad,92,4);
    check(!smg::indexRarc({bad.data(),bad.size()},entries,error), "RARC file data bounds enforced");
    bad=data; bad[128]='x'; bad[130]='x';
    check(!smg::indexRarc({bad.data(),bad.size()},entries,error), "unterminated RARC name rejected");
    bad=data; bad[120]='/';
    check(!smg::indexRarc({bad.data(),bad.size()},entries,error), "unsafe archive path rejected");
    bad=data; put32(bad,84,0x02000000);
    check(!smg::indexRarc({bad.data(),bad.size()},entries,error), "RARC directory cycle rejected");
    check(entries.empty(), "failed index never exposes partially indexed entries");

    // The disc names FloaterOtakingGalaxy.arc; the game asks for FloaterOtaKingGalaxy, as the Wii DVD allows.
    char folder[] = "/tmp/smg-archive-XXXXXX";
    check(mkdtemp(folder) != nullptr, "temporary folder");
    const std::string root = folder;
    std::filesystem::create_directory(root + "/StageData");
    FILE* file = std::fopen((root + "/StageData/FloaterOtakingGalaxy.arc").c_str(), "wb");
    check(file && std::fwrite(data.data(), 1, data.size(), file) == data.size() && !std::fclose(file), "archive written");
    std::vector<std::uint8_t> storage;
    check(smg::readArchive((root + "/stagedata/FloaterOtaKingGalaxy.arc").c_str(), storage, entries, 1 << 20, error) &&
          entries.size() == 1, "archive found whatever the case of its name");
    check(!smg::readArchive((root + "/StageData/FloaterGalaxy.arc").c_str(), storage, entries, 1 << 20, error),
          "a missing archive is still missing");
    std::filesystem::remove_all(root);
    std::printf("%u archive checks passed\n",checks);
}

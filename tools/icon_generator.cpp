#include <cstdint>
#include <fstream>
#include <string_view>
#include <vector>

namespace {

#pragma pack(push, 1)
struct IconDirectory {
    std::uint16_t reserved = 0;
    std::uint16_t type = 1;
    std::uint16_t count = 1;
};

struct IconEntry {
    std::uint8_t width = 32;
    std::uint8_t height = 32;
    std::uint8_t colors = 0;
    std::uint8_t reserved = 0;
    std::uint16_t planes = 1;
    std::uint16_t bits_per_pixel = 32;
    std::uint32_t bytes = 0;
    std::uint32_t offset = sizeof(IconDirectory) + sizeof(IconEntry);
};
#pragma pack(pop)

// Exact PNG bytes of the 32x32 icon supplied by the user. The ICO format has
// supported PNG-compressed images since Windows Vista, so no pixel conversion
// or palette change is needed.
constexpr std::string_view kPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAClklEQVR4AcSWWbajMAxE"
    "xdsY3lnEzmBldF1jE2MwEPjonFRkDajKEyd/9p8/jwW42yztGRpef919rqs2ApSdQV1U"
    "+5CHMMbwOI7mvm8ck+mHfEKXQqvZCMjR2eLssrux7jZDHkIwyPu+t6AxBJtCOcQSOtk"
    "duUrsUIAq9SW9RSbPUcgZZ8sYiGxOaBJTBw4FkKiRyfs+GDMXwabk8/mYYjNB2Uti6sB"
    "GgKbdARIlSnJ2RwQWQihLHo83Ao661OTTNB2V/RzTJOJqnQqoyc20Pvb8A2mB2Kwp4Jh"
    "8jkuvJocqynOQC1SbDyQ2ng3FIjk1qwAIS+hMWd+zz6wU9VgeuQ8RHZKWHaIAiEMYLSR"
    "8CzJptt/MnZEEoPy09A/yz2c27nKvGY9jMHyzNqnrIa7iMAynze8k4wp8C9uk3xozBL"
    "DfxLrucpKUNVEJyM2ubLPfz4lCAKR5BbDZr+3Cced9oJXiEJ5iFTBNozpDJqP7nv3akr"
    "0Ld1+vXWu8CuAALmRm2OzXNpNzCPP4jV0FZFKa1aSlT16ziQcR+1YI17Abhs64fkO6V"
    "diSNIvD1gLw3yCugLtpr8zczdzN3M1aIkqyEILx/ihj5VgrxGkuQ7txFJCinMAV7nYoI"
    "tXaFTl1EqCJ+XoLiNUoBdQ5PbyIYHtY/l5vSjU1UBfj82ZUjkngRuAX2Ik5E0CDKIIBI"
    "tgWZo7/BIUQ9V3+yF4JgEfFmAUIWUbvfhFDhzsCqNuI0MP29vrRFNwVQG0hwnfnoNx/"
    "9+Xg8dAVfhFAr0KE6Y05EVttdPQjAaqLr+F46BRqfn8VQCM1N3M3CyFEcrYDUpIliCU0"
    "hTwRAEcUwQARkDBugXzCXNc8FUAf7nsG/iUQURe9EVD3OvSvgv8AAAD//x6K7HIAAAAG"
    "SURBVAMA0lmfUMTgyGIAAAAASUVORK5CYII=";

int base64Value(char character) {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

std::vector<std::uint8_t> decodeBase64(std::string_view encoded) {
    std::vector<std::uint8_t> decoded;
    decoded.reserve(encoded.size() * 3U / 4U);
    std::uint32_t accumulator = 0;
    unsigned bits = 0;
    for (const char character : encoded) {
        if (character == '=') {
            break;
        }
        const int value = base64Value(character);
        if (value < 0) {
            continue;
        }
        accumulator = (accumulator << 6U) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<std::uint8_t>(accumulator >> bits));
            accumulator &= (1U << bits) - 1U;
        }
    }
    return decoded;
}

template <typename T>
void writeObject(std::ofstream& output, const T& object) {
    output.write(reinterpret_cast<const char*>(&object), sizeof(object));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const std::vector<std::uint8_t> png = decodeBase64(kPngBase64);
    if (png.size() != 737U) {
        return 3;
    }

    IconDirectory directory;
    IconEntry entry;
    entry.bytes = static_cast<std::uint32_t>(png.size());

    std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
    if (!output) {
        return 4;
    }
    writeObject(output, directory);
    writeObject(output, entry);
    output.write(reinterpret_cast<const char*>(png.data()),
                 static_cast<std::streamsize>(png.size()));
    return output ? 0 : 5;
}

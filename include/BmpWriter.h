#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

// Minimal 24-bit uncompressed BMP writer.
//
// Extracted from HoldGrid, which grew the original copy for its
// hold-partition debug dump. TravelGraph needs the same thing for its
// graph dump, so the writer moved here rather than being duplicated.
// Deliberately tiny: no compression, no palette, no alpha — these are
// diagnostic images a human opens once, not assets.
namespace NarrativeEngine::Bmp
{
    struct RGB
    {
        std::uint8_t r;
        std::uint8_t g;
        std::uint8_t b;
    };

    // Writes width x height pixels sampled via `pixelAt(pixelX, pixelY)`,
    // where pixelY=0 is the TOP of the image — the writer flips
    // internally for BMP's bottom-up row convention. Rows are padded to
    // 4-byte boundaries. Returns false on bad dimensions or file I/O
    // failure.
    template <class PixelFn> bool Write24(const std::filesystem::path& path, int width, int height, PixelFn pixelAt)
    {
        if (width <= 0 || height <= 0) {
            return false;
        }
        const int rowSize = ((width * 3 + 3) / 4) * 4;
        const std::uint32_t pixelDataSize = static_cast<std::uint32_t>(rowSize) * static_cast<std::uint32_t>(height);
        const std::uint32_t fileSize = 14 + 40 + pixelDataSize;

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }

        auto writeU16 = [&](std::uint16_t v) {
            std::uint8_t bytes[2] = {
                static_cast<std::uint8_t>(v & 0xFF),
                static_cast<std::uint8_t>((v >> 8) & 0xFF),
            };
            out.write(reinterpret_cast<const char*>(bytes), 2);
        };
        auto writeU32 = [&](std::uint32_t v) {
            std::uint8_t bytes[4] = {
                static_cast<std::uint8_t>(v & 0xFF),
                static_cast<std::uint8_t>((v >> 8) & 0xFF),
                static_cast<std::uint8_t>((v >> 16) & 0xFF),
                static_cast<std::uint8_t>((v >> 24) & 0xFF),
            };
            out.write(reinterpret_cast<const char*>(bytes), 4);
        };
        auto writeS32 = [&](std::int32_t v) { writeU32(static_cast<std::uint32_t>(v)); };

        // BITMAPFILEHEADER (14 bytes)
        writeU16(0x4D42); // 'BM'
        writeU32(fileSize);
        writeU16(0);
        writeU16(0);
        writeU32(54);
        // BITMAPINFOHEADER (40 bytes)
        writeU32(40);
        writeS32(width);
        writeS32(height); // positive = bottom-up rows
        writeU16(1);
        writeU16(24);
        writeU32(0); // BI_RGB
        writeU32(pixelDataSize);
        writeS32(2835); // 72 DPI in pixels/meter
        writeS32(2835);
        writeU32(0);
        writeU32(0);

        // Pixel rows: file row 0 = bottom = image y = (height-1).
        std::vector<std::uint8_t> row(static_cast<std::size_t>(rowSize), 0);
        for (int fileRow = 0; fileRow < height; ++fileRow) {
            const int imageY = height - 1 - fileRow;
            for (int x = 0; x < width; ++x) {
                const RGB c = pixelAt(x, imageY);
                // BMP pixel byte order is BGR.
                row[static_cast<std::size_t>(x) * 3 + 0] = c.b;
                row[static_cast<std::size_t>(x) * 3 + 1] = c.g;
                row[static_cast<std::size_t>(x) * 3 + 2] = c.r;
            }
            out.write(reinterpret_cast<const char*>(row.data()), rowSize);
        }
        return out.good();
    }
} // namespace NarrativeEngine::Bmp

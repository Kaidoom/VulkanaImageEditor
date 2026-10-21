#pragma once
#include <QByteArray>

namespace imageeditor::test {
// Tiny, original GIF fixture: fixed three-bit LZW codes, clearing before each
// literal. No encoder dependency or third-party image is needed by the tests.
inline QByteArray gifFixture()
{
    QByteArray bytes("GIF89a", 6);
    const auto u8 = [&](int value) { bytes.append(char(value)); };
    const auto u16 = [&](int value) { u8(value & 255); u8(value >> 8); };
    u16(4); u16(3); u8(0x81); u8(0); u8(0);
    bytes.append(QByteArray::fromHex("000000ff000000ff000000ff"));
    const auto frame = [&](int x, int y, int width, int height, int color, int disposal, int delay) {
        u8(0x21); u8(0xf9); u8(4); u8((disposal << 2) | 1);
        u16(delay); u8(0); u8(0);
        u8(0x2c); u16(x); u16(y); u16(width); u16(height); u8(0);
        u8(2);
        QByteArray codes;
        unsigned bits = 0; int count = 0;
        const auto code = [&](unsigned value) {
            bits |= value << count; count += 3;
            while (count >= 8) { codes.append(char(bits & 255)); bits >>= 8; count -= 8; }
        };
        for (int i = 0; i < width * height; ++i) { code(4); code(unsigned(color)); }
        code(5);
        if (count) codes.append(char(bits & 255));
        u8(int(codes.size())); bytes.append(codes); u8(0);
    };
    frame(0, 0, 4, 3, 1, 1, 4);  // full red, retain
    frame(1, 1, 1, 1, 2, 2, 7);  // green patch, restore background
    frame(3, 0, 1, 1, 3, 3, 1);  // blue patch, restore previous; exact 10 ms
    frame(0, 2, 1, 1, 2, 1, 12); // green patch, retain
    frame(0, 0, 1, 1, 0, 1, 0);  // transparent patch, unspecified delay
    u8(0x3b);
    return bytes;
}
}

/**
 * @file rkpluginmediaformat.h
 * @brief The numbers of the media plugin protocol's texture format messages ("internalformat", "format", "type" of the texture parameter messages).
 *
 * The plugins are separate processes and the protocol is older than the viewer's typed formats: the values on the wire are the numbers of the OpenGL enums
 * they were taken from. This header names them so that the plugins need no OpenGL header; the viewer decodes them in one place (texelFormatFromGL / pixelFormatFromGL /
 * componentTypeFromGL, rkvkformat.cpp, used by llviewermedia.cpp). Changing a value here changes the protocol.
 */

#ifndef RK_RKPLUGINMEDIAFORMAT_H
#define RK_RKPLUGINMEDIAFORMAT_H

    // pixel data layout ("format") and storage ("internalformat")
constexpr unsigned WIRE_RGB = 0x1907;
constexpr unsigned WIRE_RGBA = 0x1908;
constexpr unsigned WIRE_RGBA8 = 0x8058;
constexpr unsigned WIRE_BGRA = 0x80E1;
    // component type ("type")
constexpr unsigned WIRE_UNSIGNED_BYTE = 0x1401;
constexpr unsigned WIRE_UNSIGNED_INT_8_8_8_8_REV = 0x8367;      // 8-bit components of a packed 32 bit pixel (little endian: the bytes of a BGRA / RGBA pixel)

#endif

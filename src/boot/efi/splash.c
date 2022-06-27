/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <efi.h>
#include <efilib.h>

#include "graphics.h"
#include "splash.h"
#include "util.h"

struct bmp_file {
        char signature[2];
        uint32_t size;
        uint16_t reserved[2];
        uint32_t offset;
} _packed_;

/* we require at least BITMAPINFOHEADER, later versions are
   accepted, but their features ignored */
struct bmp_dib {
        uint32_t size;
        uint32_t x;
        uint32_t y;
        uint16_t planes;
        uint16_t depth;
        uint32_t compression;
        uint32_t image_size;
        int32_t x_pixel_meter;
        int32_t y_pixel_meter;
        uint32_t colors_used;
        uint32_t colors_important;
} _packed_;

struct bmp_map {
        uint8_t blue;
        uint8_t green;
        uint8_t red;
        uint8_t reserved;
} _packed_;

static EFI_STATUS bmp_parse_header(
                const uint8_t *bmp,
                UINTN size,
                struct bmp_dib **ret_dib,
                struct bmp_map **ret_map,
                const uint8_t **pixmap) {

        struct bmp_file *file;
        struct bmp_dib *dib;
        struct bmp_map *map;
        UINTN row_size;

        assert(bmp);
        assert(ret_dib);
        assert(ret_map);
        assert(pixmap);

        if (size < sizeof(struct bmp_file) + sizeof(struct bmp_dib))
                return EFI_INVALID_PARAMETER;

        /* check file header */
        file = (struct bmp_file *)bmp;
        if (file->signature[0] != 'B' || file->signature[1] != 'M')
                return EFI_INVALID_PARAMETER;
        if (file->size != size)
                return EFI_INVALID_PARAMETER;
        if (file->size < file->offset)
                return EFI_INVALID_PARAMETER;

        /*  check device-independent bitmap */
        dib = (struct bmp_dib *)(bmp + sizeof(struct bmp_file));
        if (dib->size < sizeof(struct bmp_dib))
                return EFI_UNSUPPORTED;

        switch (dib->depth) {
        case 1:
        case 4:
        case 8:
        case 24:
                if (dib->compression != 0)
                        return EFI_UNSUPPORTED;

                break;

        case 16:
        case 32:
                if (dib->compression != 0 && dib->compression != 3)
                        return EFI_UNSUPPORTED;

                break;

        default:
                return EFI_UNSUPPORTED;
        }

        row_size = ((UINTN) dib->depth * dib->x + 31) / 32 * 4;
        if (file->size - file->offset <  dib->y * row_size)
                return EFI_INVALID_PARAMETER;
        if (row_size * dib->y > 64 * 1024 * 1024)
                return EFI_INVALID_PARAMETER;

        /* check color table */
        map = (struct bmp_map *)(bmp + sizeof(struct bmp_file) + dib->size);
        if (file->offset < sizeof(struct bmp_file) + dib->size)
                return EFI_INVALID_PARAMETER;

        if (file->offset > sizeof(struct bmp_file) + dib->size) {
                uint32_t map_count;
                UINTN map_size;

                if (dib->colors_used)
                        map_count = dib->colors_used;
                else {
                        switch (dib->depth) {
                        case 1:
                        case 4:
                        case 8:
                                map_count = 1 << dib->depth;
                                break;

                        default:
                                map_count = 0;
                                break;
                        }
                }

                map_size = file->offset - (sizeof(struct bmp_file) + dib->size);
                if (map_size != sizeof(struct bmp_map) * map_count)
                        return EFI_INVALID_PARAMETER;
        }

        *ret_map = map;
        *ret_dib = dib;
        *pixmap = bmp + file->offset;

        return EFI_SUCCESS;
}

static void pixel_blend(uint32_t *dst, const uint32_t source) {
        uint32_t alpha, src, src_rb, src_g, dst_rb, dst_g, rb, g;

        assert(dst);

        alpha = (source & 0xff);

        /* convert src from RGBA to XRGB */
        src = source >> 8;

        /* decompose into RB and G components */
        src_rb = (src & 0xff00ff);
        src_g  = (src & 0x00ff00);

        dst_rb = (*dst & 0xff00ff);
        dst_g  = (*dst & 0x00ff00);

        /* blend */
        rb = ((((src_rb - dst_rb) * alpha + 0x800080) >> 8) + dst_rb) & 0xff00ff;
        g  = ((((src_g  -  dst_g) * alpha + 0x008000) >> 8) +  dst_g) & 0x00ff00;

        *dst = (rb | g);
}

static EFI_STATUS bmp_to_blt(
                EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buf,
                struct bmp_dib *dib,
                struct bmp_map *map,
                const uint8_t *pixmap) {

        const uint8_t *in;

        assert(buf);
        assert(dib);
        assert(map);
        assert(pixmap);

        /* transform and copy pixels */
        in = pixmap;
        for (UINTN y = 0; y < dib->y; y++) {
                EFI_GRAPHICS_OUTPUT_BLT_PIXEL *out;
                UINTN row_size;

                out = &buf[(dib->y - y - 1) * dib->x];
                for (UINTN x = 0; x < dib->x; x++, in++, out++) {
                        switch (dib->depth) {
                        case 1: {
                                for (UINTN i = 0; i < 8 && x < dib->x; i++) {
                                        out->Red = map[((*in) >> (7 - i)) & 1].red;
                                        out->Green = map[((*in) >> (7 - i)) & 1].green;
                                        out->Blue = map[((*in) >> (7 - i)) & 1].blue;
                                        out++;
                                        x++;
                                }
                                out--;
                                x--;
                                break;
                        }

                        case 4: {
                                UINTN i;

                                i = (*in) >> 4;
                                out->Red = map[i].red;
                                out->Green = map[i].green;
                                out->Blue = map[i].blue;
                                if (x < (dib->x - 1)) {
                                        out++;
                                        x++;
                                        i = (*in) & 0x0f;
                                        out->Red = map[i].red;
                                        out->Green = map[i].green;
                                        out->Blue = map[i].blue;
                                }
                                break;
                        }

                        case 8:
                                out->Red = map[*in].red;
                                out->Green = map[*in].green;
                                out->Blue = map[*in].blue;
                                break;

                        case 16: {
                                uint16_t i = *(uint16_t *) in;

                                out->Red = (i & 0x7c00) >> 7;
                                out->Green = (i & 0x3e0) >> 2;
                                out->Blue = (i & 0x1f) << 3;
                                in += 1;
                                break;
                        }

                        case 24:
                                out->Red = in[2];
                                out->Green = in[1];
                                out->Blue = in[0];
                                in += 2;
                                break;

                        case 32: {
                                uint32_t i = *(uint32_t *) in;

                                pixel_blend((uint32_t *)out, i);

                                in += 3;
                                break;
                        }
                        }
                }

                /* add row padding; new lines always start at 32 bit boundary */
                row_size = in - pixmap;
                in += ((row_size + 3) & ~3) - row_size;
        }

        return EFI_SUCCESS;
}

EFI_STATUS graphics_splash(const uint8_t *content, UINTN len, const EFI_GRAPHICS_OUTPUT_BLT_PIXEL *background) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL pixel = {};
        EFI_GRAPHICS_OUTPUT_PROTOCOL *GraphicsOutput = NULL;
        struct bmp_dib *dib;
        struct bmp_map *map;
        const uint8_t *pixmap;
        _cleanup_freepool_ void *blt = NULL;
        UINTN x_pos = 0;
        UINTN y_pos = 0;
        EFI_STATUS err;

        if (len == 0)
                return EFI_SUCCESS;

        assert(content);

        if (!background) {
                if (strcaseeq16(u"Apple", ST->FirmwareVendor)) {
                        pixel.Red = 0xc0;
                        pixel.Green = 0xc0;
                        pixel.Blue = 0xc0;
                }
                background = &pixel;
        }

        err = BS->LocateProtocol(&GraphicsOutputProtocol, NULL, (void **) &GraphicsOutput);
        if (err != EFI_SUCCESS)
                return err;

        err = bmp_parse_header(content, len, &dib, &map, &pixmap);
        if (err != EFI_SUCCESS)
                return err;

        if (dib->x < GraphicsOutput->Mode->Info->HorizontalResolution)
                x_pos = (GraphicsOutput->Mode->Info->HorizontalResolution - dib->x) / 2;
        if (dib->y < GraphicsOutput->Mode->Info->VerticalResolution)
                y_pos = (GraphicsOutput->Mode->Info->VerticalResolution - dib->y) / 2;

        err = GraphicsOutput->Blt(
                        GraphicsOutput, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)background,
                        EfiBltVideoFill, 0, 0, 0, 0,
                        GraphicsOutput->Mode->Info->HorizontalResolution,
                        GraphicsOutput->Mode->Info->VerticalResolution, 0);
        if (err != EFI_SUCCESS)
                return err;

        /* EFI buffer */
        blt = xnew(EFI_GRAPHICS_OUTPUT_BLT_PIXEL, dib->x * dib->y);

        err = GraphicsOutput->Blt(
                        GraphicsOutput, blt,
                        EfiBltVideoToBltBuffer, x_pos, y_pos, 0, 0,
                        dib->x, dib->y, 0);
        if (err != EFI_SUCCESS)
                return err;

        err = bmp_to_blt(blt, dib, map, pixmap);
        if (err != EFI_SUCCESS)
                return err;

        err = graphics_mode(TRUE);
        if (err != EFI_SUCCESS)
                return err;

        return GraphicsOutput->Blt(
                        GraphicsOutput, blt,
                        EfiBltBufferToVideo, 0, 0, x_pos, y_pos,
                        dib->x, dib->y, 0);
}

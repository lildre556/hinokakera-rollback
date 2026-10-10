#include "d3d.h"

#define COBJMACROS
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "log.h"

static void put_u16(FILE *f, unsigned v) {
    fputc(v & 0xff, f);
    fputc((v >> 8) & 0xff, f);
}

static void put_u32(FILE *f, unsigned v) {
    put_u16(f, v & 0xffff);
    put_u16(f, v >> 16);
}

bool d3d_screenshot(void *device_ptr, const char *path) {
    IDirect3DDevice9 *dev = device_ptr;
    if (!dev) {
        return false;
    }
    IDirect3DSurface9 *bb = NULL, *copy = NULL;
    bool ok = false;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        log_printf("shot: GetBackBuffer failed");
        return false;
    }
    D3DSURFACE_DESC desc;
    IDirect3DSurface9_GetDesc(bb, &desc);
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                                            &copy, NULL))) {
        log_printf("shot: CreateOffscreenPlainSurface failed (format %d)", desc.Format);
        goto out;
    }
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, copy))) {
        log_printf("shot: GetRenderTargetData failed");
        goto out;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(copy, &lr, NULL, D3DLOCK_READONLY))) {
        log_printf("shot: LockRect failed");
        goto out;
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        unsigned w = desc.Width, h = desc.Height;
        unsigned row = (w * 3 + 3) & ~3u;
        fputc('B', f);
        fputc('M', f);
        put_u32(f, 54 + row * h);
        put_u32(f, 0);
        put_u32(f, 54);
        put_u32(f, 40);
        put_u32(f, w);
        put_u32(f, h);
        put_u16(f, 1);
        put_u16(f, 24);
        put_u32(f, 0);
        put_u32(f, row * h);
        put_u32(f, 2835);
        put_u32(f, 2835);
        put_u32(f, 0);
        put_u32(f, 0);
        unsigned char *line = calloc(1, row);
        for (unsigned y = 0; y < h; y++) {
            const unsigned char *src = (const unsigned char *)lr.pBits + (size_t)(h - 1 - y) * lr.Pitch;
            for (unsigned x = 0; x < w; x++) {
                unsigned r, g, b;
                if (desc.Format == D3DFMT_R5G6B5) {
                    unsigned v = ((const unsigned short *)src)[x];
                    r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
                } else if (desc.Format == D3DFMT_X1R5G5B5 || desc.Format == D3DFMT_A1R5G5B5) {
                    unsigned v = ((const unsigned short *)src)[x];
                    r = ((v >> 10) & 31) << 3, g = ((v >> 5) & 31) << 3, b = (v & 31) << 3;
                } else {
                    unsigned v = ((const unsigned *)src)[x];
                    r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
                }
                line[x * 3] = b;
                line[x * 3 + 1] = g;
                line[x * 3 + 2] = r;
            }
            fwrite(line, 1, row, f);
        }
        free(line);
        fclose(f);
        ok = true;
    }
    IDirect3DSurface9_UnlockRect(copy);
out:
    if (copy) {
        IDirect3DSurface9_Release(copy);
    }
    if (bb) {
        IDirect3DSurface9_Release(bb);
    }
    return ok;
}

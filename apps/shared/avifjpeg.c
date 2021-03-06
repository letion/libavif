// Copyright 2020 Joe Drago. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "avifjpeg.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeglib.h"

#include "iccjpeg.h"

struct my_error_mgr
{
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};
typedef struct my_error_mgr * my_error_ptr;
static void my_error_exit(j_common_ptr cinfo)
{
    my_error_ptr myerr = (my_error_ptr)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(myerr->setjmp_buffer, 1);
}

// Note on setjmp() and volatile variables:
//
// K & R, The C Programming Language 2nd Ed, p. 254 says:
//   ... Accessible objects have the values they had when longjmp was called,
//   except that non-volatile automatic variables in the function calling setjmp
//   become undefined if they were changed after the setjmp call.
//
// Therefore, 'iccData' is declared as volatile. 'rgb' should be declared as
// volatile, but doing so would be inconvenient (try it) and since it is a
// struct, the compiler is unlikely to put it in a register. 'ret' does not need
// to be declared as volatile because it is not modified between setjmp and
// longjmp. But GCC's -Wclobbered warning may have trouble figuring that out, so
// we preemptively declare it as volatile.

avifBool avifJPEGRead(const char * inputFilename, avifImage * avif, avifPixelFormat requestedFormat, uint32_t requestedDepth)
{
    volatile avifBool ret = AVIF_FALSE;
    uint8_t * volatile iccData = NULL;

    avifRGBImage rgb;
    memset(&rgb, 0, sizeof(avifRGBImage));

    FILE * f = fopen(inputFilename, "rb");
    if (!f) {
        fprintf(stderr, "Can't open JPEG file for read: %s\n", inputFilename);
        return ret;
    }

    struct my_error_mgr jerr;
    struct jpeg_decompress_struct cinfo;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;
    if (setjmp(jerr.setjmp_buffer)) {
        goto cleanup;
    }

    jpeg_create_decompress(&cinfo);

    setup_read_icc_profile(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int row_stride = cinfo.output_width * cinfo.output_components;
    JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);

    uint8_t * iccDataTmp;
    unsigned int iccDataLen;
    if (read_icc_profile(&cinfo, &iccDataTmp, &iccDataLen)) {
        iccData = iccDataTmp;
        avifImageSetProfileICC(avif, iccDataTmp, (size_t)iccDataLen);
    }

    avif->width = cinfo.output_width;
    avif->height = cinfo.output_height;
    avif->yuvFormat = requestedFormat;
    avif->depth = requestedDepth ? requestedDepth : 8;
    // JPEG doesn't have alpha. Prevent confusion.
    avif->alphaPremultiplied = AVIF_FALSE;
    avifRGBImageSetDefaults(&rgb, avif);
    rgb.format = AVIF_RGB_FORMAT_RGB;
    rgb.depth = 8;
    avifRGBImageAllocatePixels(&rgb);

    int row = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, buffer, 1);
        uint8_t * pixelRow = &rgb.pixels[row * rgb.rowBytes];
        memcpy(pixelRow, buffer[0], rgb.rowBytes);
        ++row;
    }
    if (avifImageRGBToYUV(avif, &rgb) != AVIF_RESULT_OK) {
        fprintf(stderr, "Conversion to YUV failed: %s\n", inputFilename);
        goto cleanup;
    }

    jpeg_finish_decompress(&cinfo);
    ret = AVIF_TRUE;
cleanup:
    jpeg_destroy_decompress(&cinfo);
    if (f) {
        fclose(f);
    }
    free(iccData);
    avifRGBImageFreePixels(&rgb);
    return ret;
}

#if !defined(JCS_ALPHA_EXTENSIONS)
// this is only for removing alpha when processing non-premultiplied image.
static void avifRGBAToRGB(const avifRGBImage * src, avifRGBImage * dst)
{
    dst->width = src->width;
    dst->height = src->height;
    dst->format = AVIF_RGB_FORMAT_RGB;
    dst->depth = 8;

    avifRGBImageAllocatePixels(dst);

    for (uint32_t j = 0; j < src->height; ++j) {
        uint8_t * srcRow = &src->pixels[j * src->rowBytes];
        uint8_t * dstRow = &src->pixels[j * dst->rowBytes];
        for (uint32_t i = 0; i < src->width; ++i) {
            uint8_t * srcPixel = &srcRow[i * 4];
            uint8_t * dstPixel = &dstRow[i * 3];
            memcpy(dstPixel, srcPixel, 3);
        }
    }
}
#endif

avifBool avifJPEGWrite(const char * outputFilename, avifImage * avif, int jpegQuality, avifChromaUpsampling chromaUpsampling)
{
    avifBool ret = AVIF_FALSE;
    FILE * f = NULL;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    avifRGBImage rgb;
    avifRGBImage rgbPremultiplied;
    avifRGBImageSetDefaults(&rgb, avif);
    avifRGBImageSetDefaults(&rgbPremultiplied, avif);
    rgb.format = avif->alphaPremultiplied ? AVIF_RGB_FORMAT_RGB : AVIF_RGB_FORMAT_RGBA;
    rgb.chromaUpsampling = chromaUpsampling;
    rgb.depth = 8;
    // always get premultiplied result.
    // This will give natural appearance to output JPG image.
    rgb.alphaPremultiplied = AVIF_TRUE;
    avifRGBImageAllocatePixels(&rgb);
    if (avifImageYUVToRGB(avif, &rgb) != AVIF_RESULT_OK) {
        fprintf(stderr, "Conversion to RGB failed: %s\n", outputFilename);
        goto cleanup;
    }

    // libjpeg-turbo accepts RGBA input, so do less if possible
#if !defined(JCS_ALPHA_EXTENSIONS)
    if (!avif->alphaPremultiplied) {
        if (avifRGBImagePremultiplyAlpha(&rgb) != AVIF_RESULT_OK) {
            fprintf(stderr, "Conversion to RGB failed: %s\n", outputFilename);
            goto cleanup;
        }
        avifRGBAToRGB(&rgb, &rgbPremultiplied);
        avifRGBImageFreePixels(&rgb);
    }
#endif

    f = fopen(outputFilename, "wb");
    if (!f) {
        fprintf(stderr, "Can't open JPEG file for write: %s\n", outputFilename);
        goto cleanup;
    }

    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = avif->width;
    cinfo.image_height = avif->height;
#if defined(JCS_ALPHA_EXTENSIONS)
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_EXT_RGBX;
#else
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
#endif
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, jpegQuality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    if (avif->icc.data && (avif->icc.size > 0)) {
        write_icc_profile(&cinfo, avif->icc.data, (unsigned int)avif->icc.size);
    }

#if defined(JCS_ALPHA_EXTENSIONS)
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = &rgb.pixels[cinfo.next_scanline * rgb.rowBytes];
        (void)jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }
#else
    if (avif->alphaPremultiplied) {
        while (cinfo.next_scanline < cinfo.image_height) {
            row_pointer[0] = &rgb.pixels[cinfo.next_scanline * rgb.rowBytes];
            (void)jpeg_write_scanlines(&cinfo, row_pointer, 1);
        }
    } else {
        while (cinfo.next_scanline < cinfo.image_height) {
            row_pointer[0] = &rgbPremultiplied.pixels[cinfo.next_scanline * rgbPremultiplied.rowBytes];
            (void)jpeg_write_scanlines(&cinfo, row_pointer, 1);
        }
    }
#endif

    jpeg_finish_compress(&cinfo);
    ret = AVIF_TRUE;
    printf("Wrote JPEG: %s\n", outputFilename);
cleanup:
    if (f) {
        fclose(f);
    }
    jpeg_destroy_compress(&cinfo);
    avifRGBImageFreePixels(&rgb);
    avifRGBImageFreePixels(&rgbPremultiplied);
    return ret;
}

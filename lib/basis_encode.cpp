/* -*- tab-width: 4; -*- */
/* vi: set sw=2 ts=4 expandtab: */

/*
 * ©2019 Khronos Group, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @internal
 * @file basis_encode.c
 * @~English
 *
 * @brief Functions for supercompressing a texture with Basis Universal.
 *
 * This is where two worlds collide. Ugly!
 *
 * @author Mark Callow, www.edgewise-consulting.com
 */

#include <inttypes.h>
#include <KHR/khr_df.h>

#include "ktx.h"
#include "ktxint.h"
#include "texture2.h"
#include "vkformat_enum.h"
#include "vk_format.h"
#include "basis_sgd.h"
#include "basisu/basisu_comp.h"
#include "basisu/transcoder/basisu_file_headers.h"
#include "basisu/transcoder/basisu_transcoder.h"
#include "dfdutils/dfd.h"

using namespace basisu;
using namespace basist;

enum swizzle_e {
    R = 0,
    G = 1,
    B = 2,
    A = 3,
    ZERO = 4,
    ONE = 5,
};

typedef void
(* PFNBUCOPYCB)(uint8_t* rgbadst, uint8_t* rgbasrc, uint32_t src_len,
                ktx_size_t image_size, swizzle_e swizzle[4]);

// All callbacks expect source images to have no row padding and expect
// component size to be 8 bits.

static void
copy_rgba_to_rgba(uint8_t* rgbadst, uint8_t* rgbasrc, uint32_t src_len,
                  ktx_size_t image_size, swizzle_e swizzle[4])
{
    memcpy(rgbadst, rgbasrc, image_size);
}

// Copy rgb to rgba. No swizzle.
static void
copy_rgb_to_rgba(uint8_t* rgbadst, uint8_t* rgbsrc, uint32_t src_len,
                 ktx_size_t image_size, swizzle_e swizzle[4])
{
    for (ktx_size_t i = 0; i < image_size; i += 3) {
        memcpy(rgbadst, rgbsrc, 3);
        rgbadst[3] = 0xff; // Convince Basis there is no alpha.
        rgbadst += 4; rgbsrc += 3;
    }
}

static void
swizzle_to_rgba(uint8_t* rgbadst, uint8_t* rgbasrc, uint32_t src_len,
                ktx_size_t image_size, swizzle_e swizzle[4])
{
    for (ktx_size_t i = 0; i < image_size; i += src_len) {
        for (uint32_t c = 0; c < src_len; c++) {
            switch (swizzle[c]) {
              case R:
                rgbadst[c] = rgbasrc[0];
                break;
              case G:
                rgbadst[c] = rgbasrc[1];
                break;
              case B:
                rgbadst[c] = rgbasrc[2];
                break;
              case A:
                rgbadst[c] = rgbasrc[3];
                break;
              case ZERO:
                rgbadst[c] = 0x00;
                break;
              case ONE:
                rgbadst[i+c] = 0xff;
                break;
              default:
                assert(false);
            }
        }
        rgbadst +=4; rgbasrc += src_len;
    }
}

#if 0
static void
swizzle_rgba_to_rgba(uint8_t* rgbadst, uint8_t* rgbasrc, ktx_size_t image_size,
                     swizzle_e swizzle[4])
{
    for (ktx_size_t i = 0; i < image_size; i += 4) {
        for (uint32_t c = 0; c < 4; c++) {
            switch (swizzle[c]) {
              case 0:
                rgbadst[c] = rgbasrc[0];
                break;
              case 1:
                rgbadst[c] = rgbasrc[1];
                break;
              case 2:
                rgbadst[c] = rgbasrc[2];
                break;
              case 3:
                rgbadst[c] = rgbasrc[3];
                break;
              case 4:
                rgbadst[c] = 0x00;
                break;
              case 5:
                rgbadst[i+c] = 0xff;
                break;
              default:
                assert(false);
            }
        }
        rgbadst +=4; rgbasrc += 4;
    }
}

static void
swizzle_rgb_to_rgba(uint8_t* rgbadst, uint8_t* rgbsrc, ktx_size_t image_size,
                     swizzle_e swizzle[4])
{
    for (ktx_size_t i = 0; i < image_size; i += 3) {
        for (uint32_t c = 0; c < 3; c++) {
            switch (swizzle[c]) {
              case 0:
                rgbadst[c] = rgbsrc[0];
                break;
              case 1:
                rgbadst[c] = rgbsrc[i];
                break;
              case 2:
                rgbadst[c] = rgbsrc[2];
                break;
              case 3:
                assert(false); // Shouldn't happen for an RGB texture.
                break;
              case 4:
                rgbadst[c] = 0x00;
                break;
              case 5:
                rgbadst[c] = 0xff;
                break;
              default:
                assert(false);
            }
        }
        rgbadst +=4; rgbsrc += 3;
    }
}

static void
swizzle_rg_to_rgb_a(uint8_t* rgbadst, uint8_t* rgsrc, ktx_size_t image_size,
                    swizzle_e swizzle[4])
{
    for (ktx_size_t i = 0; i < image_size; i += 2) {
        for (uint32_t c = 0; c < 2; c++) {
          switch (swizzle[c]) {
              case 0:
                rgbadst[c] = rgsrc[0];
                break;
              case 1:
                rgbadst[c] = rgsrc[1];
                break;
              case 2:
                 assert(false); // Shouldn't happen for an RG texture.
                 break;
              case 3:
                assert(false); // Shouldn't happen for an RG texture.
                break;
              case 4:
                rgbadst[c] = 0x00;
                break;
              case 5:
                rgbadst[c] = 0xff;
                break;
              default:
                assert(false);
            }
        }
    }
}
#endif

// Rewrite DFD changing it to unsized. Account for the Basis compressor
// not including an all 1's alpha channel, which would have been removed before
// encoding and supercompression, by looking at hasAlpha.
static KTX_error_code
ktxTexture2_rewriteDfd(ktxTexture2* This, bool hasAlpha)
{
    uint32_t* cdfd = This->pDfd;
    uint32_t* cbdb = cdfd + 1;
    uint32_t newSampleCount = KHR_DFDSAMPLECOUNT(cbdb);

    if (newSampleCount == 4 && !hasAlpha)
        newSampleCount = 3;

    uint32_t ndbSize = KHR_DF_WORD_SAMPLESTART
                       + newSampleCount * KHR_DF_WORD_SAMPLEWORDS;
    ndbSize *= sizeof(uint32_t);
    uint32_t ndfdSize = ndbSize + 1 * sizeof(uint32_t);
    uint32_t* ndfd = (uint32_t *)malloc(ndfdSize);
    uint32_t* nbdb = ndfd + 1;

    if (!ndfd)
        return KTX_OUT_OF_MEMORY;

    // Copy the basic dfd + wanted samples.
    memcpy(ndfd, cdfd, ndfdSize);
    if (ndfdSize != *cdfd) {
        // Set the size of the new DFD.
        *ndfd = ndfdSize;
        // And the descriptor block size
        KHR_DFDSETVAL(nbdb, DESCRIPTORBLOCKSIZE, ndbSize);
    }

    // Show it describes an unsized format.
    nbdb[KHR_DF_WORD_BYTESPLANE0] = 0;
    nbdb[KHR_DF_WORD_BYTESPLANE4] = 0;

    // Set the following to 0 as they have no meaning within the BasisU
    // encoded data and what they will be after inflation depends on the
    // transcode target.
    nbdb[KHR_DF_WORD_TEXELBLOCKDIMENSION0] = 0;
    for (uint32_t sample = 0; sample < newSampleCount; sample++) {
        KHR_DFDSETSVAL(nbdb, sample, BITOFFSET, 0);
        KHR_DFDSETSVAL(nbdb, sample, BITLENGTH, 0);
        KHR_DFDSETSVAL(nbdb, sample, SAMPLELOWER, 0);
        KHR_DFDSETSVAL(nbdb, sample, SAMPLEUPPER, 0);
    }

    This->pDfd = ndfd;
    free(cdfd);
    return KTX_SUCCESS;
}

/**
 * @memberof ktxTexture2
 * @ingroup writer
 * @~English
 * @brief Supercompress a KTX2 texture with uncpompressed images.
 *
 * The images are encoded to ETC1S block-compressed format and supercompressed
 * with Basis Universal. The encoded images replace the original images and the
 * texture's fields including the DFD are modified to reflect the new state.
 *
 * Such textures must be transcoded to a desired target block compressed format
 * before they can be uploaded to a GPU via a graphics API.
 *
 * @sa ktxTexture2_TranscodeBasis().
 *
 * @param[in]   This   pointer to the ktxTexture2 object of interest.
 * @param[in]   params pointer to Basis params object.
 *
 * @return      KTX_SUCCESS on success, other KTX_* enum values on error.
 *
 * @exception KTX_INVALID_OPERATION
 *                              The texture is already supercompressed.
 * @exception KTX_INVALID_OPERATION
 *                              The texture's image are in a block compressed
 *                              format.
 * @exception KTX_INVALID_OPERATION
 *                              The texture's images are 1D. Only 2D images can
 *                              be supercompressed.
 * @exception KTX_OUT_OF_MEMORY Not enough memory to carry out supercompression.
 */
extern "C" KTX_error_code
ktxTexture2_CompressBasisEx(ktxTexture2* This, ktxBasisParams* params)
{
    KTX_error_code result;

    if (!params)
        return KTX_INVALID_VALUE;

    if (params->structSize != sizeof(struct ktxBasisParams))
        return KTX_INVALID_VALUE;

    if (This->supercompressionScheme != KTX_SUPERCOMPRESSION_NONE)
        return KTX_INVALID_OPERATION; // Can't apply multiple schemes.

    if (This->isCompressed)
        return KTX_INVALID_OPERATION;  // Basis can't be applied to compression
                                       // types other than ETC1S and underlying
                                       // Basis software does ETC1S encoding &
                                       // Basis supercompression together.

    if (This->_protected->_formatSize.flags & KTX_FORMAT_SIZE_PACKED_BIT)
        return KTX_INVALID_OPERATION;

    // Basic descriptor block begins after the total size field.
    const uint32_t* BDB = This->pDfd+1;

    uint32_t num_components, component_size;
    getDFDComponentInfoUnpacked(This->pDfd, &num_components, &component_size);

    if (component_size != 1)
        return KTX_INVALID_OPERATION; // ETC/Basis must have 8-bit components.

    if (params->separateRGToRGB_A && num_components == 1)
        return KTX_INVALID_OPERATION;

    if (This->pData == NULL) {
        result = ktxTexture2_LoadImageData(This, NULL, 0);
        if (result != KTX_SUCCESS)
            return result;
    }

    basis_compressor_params cparams;
    cparams.m_read_source_images = false; // Don't read from source files.
    cparams.m_write_output_basis_files = false; // Don't write output files.

    //
    // Calculate number of images
    //
    uint32_t layersFaces = This->numLayers * This->numFaces;
    uint32_t num_images = 0;
    for (uint32_t level = 1; level <= This->numLevels; level++) {
        // NOTA BENE: numFaces * depth is only reasonable because they can't
        // both be > 1. I.e there are no 3d cubemaps.
        num_images += layersFaces * MAX(This->baseDepth >> (level - 1), 1);
    }

    //
    // Copy images into compressor parameters.
    //
    // Darn it! m_source_images is a vector of an internal image class which
    // has its own array of RGBA-only pixels. Pending modifications to the
    // basisu code we'll have to copy in the images.
    cparams.m_source_images.resize(num_images);
    std::vector<image>::iterator iit = cparams.m_source_images.begin();

    swizzle_e meta_mapping[4] = {};
    // Since we have to copy the data into the vector image anyway do the
    // separation here to avoid another loop over the image inside
    // basis_compressor.
    swizzle_e rg_to_rgba_mapping[4] = { R, R, R, G };
    swizzle_e r_to_rgba_mapping[4] = { R, R, R, ONE };
    swizzle_e* comp_mapping = 0;
    if (params->preSwizzle) {
        ktx_uint32_t swizzleLen;
        char* swizzleStr;

        result = ktxHashList_FindValue(&This->kvDataHead, KTX_SWIZZLE_KEY,
                                       &swizzleLen, (void**)&swizzleStr);
        if (result == KTX_SUCCESS) {
            // swizzleLen should be 5. 4 plus terminating NUL.
            // When move this to constructor add a check.
            // Also need to check that swizzle is 0 for missing color
            // components and 1 for missing alpha components.
            if ((num_components == 2 && strncmp(swizzleStr, "rg01", 4U))
                || (num_components == 3 && strncmp(swizzleStr, "rgb1", 4U))
                || (num_components == 4 && strncmp(swizzleStr, "rgba", 4U))) {
                for (int i = 0; i < 4; i++) {
                    switch (swizzleStr[i]) {
                      case 'r': meta_mapping[i] = R; break;
                      case 'g': meta_mapping[i] = G; break;
                      case 'b': meta_mapping[i] = B; break;
                      case 'a': meta_mapping[i] = A; break;
                      case '0': meta_mapping[i] = ZERO; break;
                      case '1': meta_mapping[i] = ONE; break;
                    }
                }
                comp_mapping = meta_mapping;
            }
        }
    }

    // There's no other way so sensibly handle 2-component textures.
    if (num_components == 2 || params->separateRGToRGB_A)
        comp_mapping = rg_to_rgba_mapping;

    if (num_components == 1)
        comp_mapping = r_to_rgba_mapping;

    PFNBUCOPYCB copycb;
    if (comp_mapping) {
        copycb = swizzle_to_rgba;
    } else {
        switch (num_components) {
          case 4: copycb = copy_rgba_to_rgba; break;
          case 3: copycb = copy_rgb_to_rgba; break;
          default: assert(false);
        }
    }

    // NOTA BENE: Mipmap levels are ordered from largest to smallest in .basis.
    for (uint32_t level = 0; level < This->numLevels; level++) {
        uint32_t width = MAX(1, This->baseWidth >> level);
        uint32_t height = MAX(1, This->baseHeight >> level);
        uint32_t depth = MAX(1, This->baseDepth >> level);
        ktx_size_t image_size = ktxTexture2_GetImageSize(This, level);
        for (uint32_t layer = 0; layer < This->numLayers; layer++) {
            uint32_t faceSlices = This->numFaces == 1 ? depth : This->numFaces;
            for (ktx_uint32_t slice = 0; slice < faceSlices; slice++) {
                ktx_size_t offset;
                ktxTexture2_GetImageOffset(This, level, layer, slice, &offset);
                iit->resize(width, height);
                copycb((uint8_t*)iit->get_ptr(), This->pData + offset,
                        component_size * num_components, image_size,
                        comp_mapping);
                ++iit;
            }
        }
    }

    free(This->pData); // No longer needed. Reduce memory footprint.
    This->pData = NULL;
    This->dataSize = 0;

    //
    // Setup rest of compressor parameters
    //
    ktx_uint32_t transfer = KHR_DFDVAL(BDB, TRANSFER);
    if (transfer == KHR_DF_TRANSFER_SRGB)
        cparams.m_perceptual = true;
    else
        cparams.m_perceptual = false;

    cparams.m_mip_gen = false; // We provide the mip levels.

    ktx_uint32_t countThreads = params->threadCount;
    if (countThreads < 1)
        countThreads = 1;

    job_pool jpool(countThreads);
    cparams.m_pJob_pool = &jpool;

    // Defaults to BASISU_DEFAULT_COMPRESSION_LEVEL
    if (params->compressionLevel)
        cparams.m_compression_level = params->compressionLevel;

    // There's no default for m_quality_level. Mimic basisu_tool.
    if (params->qualityLevel != 0) {
        cparams.m_max_endpoint_clusters = 0;
        cparams.m_max_selector_clusters = 0;
        cparams.m_quality_level = params->qualityLevel;
    } else if (!params->maxEndpoints || !params->maxSelectors) {
        cparams.m_max_endpoint_clusters = 0;
        cparams.m_max_selector_clusters = 0;
        cparams.m_quality_level = 128;
    } else {
        cparams.m_max_endpoint_clusters = params->maxEndpoints;
        cparams.m_max_selector_clusters = params->maxSelectors;
        // cparams.m_quality_level = -1; // Default setting.
    }

    if (params->endpointRDOThreshold > 0)
        cparams.m_endpoint_rdo_thresh = params->endpointRDOThreshold;
    if (params->selectorRDOThreshold > 0)
        cparams.m_selector_rdo_thresh = params->selectorRDOThreshold;

    if (params->normalMap) {
        cparams.m_no_endpoint_rdo = true;
        cparams.m_no_selector_rdo = true;
    } else {
        cparams.m_no_endpoint_rdo = params->noEndpointRDO;
        cparams.m_no_selector_rdo = params->noSelectorRDO;
    }

    // Why's there no default for this? I have no idea.
    basist::etc1_global_selector_codebook sel_codebook(basist::g_global_selector_cb_size,
                                                       basist::g_global_selector_cb);
    cparams.m_pSel_codebook = &sel_codebook;

    // Flip images across Y axis
    // cparams.m_y_flip = false; // Let tool, e.g. toktx do its own yflip so
    // ktxTexture is consistent.

    // Output debug information during compression
    //cparams.m_debug = true;

    // m_debug_images is pretty slow
    //cparams.m_debug_images = true;

    // Split the R channel to RGB and the G channel to alpha. We do the
    // seperation in this func (see above) so leave this at its default, false.
    //bool_param<false> m_seperate_rg_to_color_alpha;

    // m_tex_type, m_userdata0, m_userdata1, m_framerate - These fields go
    // directly into the Basis file header.
    //
    // Set m_tex_type to cBASISTexType2D as any other setting is likely to
    // cause validity checks, that the encoder performs on its results, to
    // fail. The checks only work properly when the encoder generates mipmaps
    // itself and are oriented to ensuring the .basis file is sensible.
    // Underlying compression works fine and we already know what level, layer
    // and face/slice each image belongs too.
    //
    cparams.m_tex_type = cBASISTexType2D;

    // TODO When video support is added, may need to set m_tex_type to this
    // if video.
    //cBASISTexTypeVideoFrames
    // and set cparams.m_us_per_frame;

#define DUMP_BASIS_FILE 0
#if DUMP_BASIS_FILE
    cparams.m_out_filename = "ktxtest.basis";
    cparams.m_write_output_basis_files = true;
#endif

    basis_compressor c;

    // init() only returns false if told to read source image files and the
    // list of files is empty.
    (void)c.init(cparams);
    //enable_debug_printf(true);
    basis_compressor::error_code ec = c.process();

    if (ec != basis_compressor::cECSuccess) {
        // We should be sending valid 2d arrays, cubemaps or video ...
        assert(ec != basis_compressor::cECFailedValidating);
        // Do something sensible with other errors
        return KTX_INVALID_OPERATION;
#if 0
        switch (ec) {
                case basis_compressor::cECFailedReadingSourceImages:
                {
                    error_printf("Compressor failed reading a source image!\n");

                    if (opts.m_individual)
                        exit_flag = false;

                    break;
                }
                case basis_compressor::cECFailedFrontEnd:
                    error_printf("Compressor frontend stage failed!\n");
                    break;
                case basis_compressor::cECFailedFontendExtract:
                    error_printf("Compressor frontend data extraction failed!\n");
                    break;
                case basis_compressor::cECFailedBackend:
                    error_printf("Compressor backend stage failed!\n");
                    break;
                case basis_compressor::cECFailedCreateBasisFile:
                    error_printf("Compressor failed creating Basis file data!\n");
                    break;
                case basis_compressor::cECFailedWritingOutput:
                    error_printf("Compressor failed writing to output Basis file!\n");
                    break;
                default:
                    error_printf("basis_compress::process() failed!\n");
                    break;
            }
        }
        return KTX_WHAT_ERROR?;
#endif
    }

#if DUMP_BASIS_FILE
    return KTX_UNSUPPORTED_FEATURE;
#endif

    //
    // Compression successful. Now we have to unpick the basis output and
    // copy the info and images to This texture.
    //

    const uint8_vec& bf = c.get_output_basis_file();
    const basis_file_header& bfh = *reinterpret_cast<const basis_file_header*>(bf.data());
    uint8_t* bgd;
    uint32_t bgd_size;
    uint32_t image_desc_size;

    assert(bfh.m_total_images == num_images);

    //
    // Allocate supercompression global data and write its header.
    //
    image_desc_size = sizeof(ktxBasisImageDesc);

    bgd_size = sizeof(ktxBasisGlobalHeader)
             + image_desc_size * num_images
             + bfh.m_endpoint_cb_file_size + bfh.m_selector_cb_file_size
             + bfh.m_tables_file_size;
    bgd = new ktx_uint8_t[bgd_size];
    ktxBasisGlobalHeader& bgdh = *reinterpret_cast<ktxBasisGlobalHeader*>(bgd);
    // Get the flags that are set while ensuring we don't get
    // cBASISHeaderFlagYFlipped
    bgdh.globalFlags = bfh.m_flags & ~cBASISHeaderFlagYFlipped;
    bgdh.endpointCount = bfh.m_total_endpoints;
    bgdh.endpointsByteLength = bfh.m_endpoint_cb_file_size;
    bgdh.selectorCount = bfh.m_total_selectors;
    bgdh.selectorsByteLength = bfh.m_selector_cb_file_size;
    bgdh.tablesByteLength = bfh.m_tables_file_size;
    bgdh.extendedByteLength = 0;

    //
    // Write the index of slice descriptions to the global data.
    //

    ktxTexture2_private& priv = *This->_private;
    uint32_t base_offset = bfh.m_slice_desc_file_ofs;
    const basis_slice_desc* slice
                = reinterpret_cast<const basis_slice_desc*>(&bf[base_offset]);
    ktxBasisImageDesc* kimages = BGD_IMAGE_DESCS(bgd);

    // 3 things to remember about offsets:
    //    1. levelIndex offsets at this point are relative to This->pData;
    //    2. In the ktx image descriptors, slice offsets are relative to the
    //       start of the mip level;
    //    3. basis_slice_desc offsets are relative to the end of the basis
    //       header. Hence base_offset set above is used to rebase offsets
    //       relative to the start of the slice data.

    // Assumption here is that slices produced by the compressor are in the
    // same order as we passed them in above, i.e. ordered by mip level.
    // Note also that slice->m_level_index is always 0, unless the compressor
    // generated mip levels, so essentially useless. Alpha slices are always
    // the odd numbered slices.
    std::vector<uint32_t> level_file_offsets(This->numLevels);
    uint32_t image_data_size = 0, image = 0;
    for (uint32_t level = 0; level < This->numLevels; level++) {
        uint32_t depth = MAX(1, This->baseDepth >> level);
        uint32_t level_byte_length = 0;

      assert(!(slice->m_flags & cSliceDescFlagsHasAlpha));
        level_file_offsets[level] = slice->m_file_ofs;
        for (uint32_t layer = 0; layer < This->numLayers; layer++) {
            uint32_t faceSlices = This->numFaces == 1 ? depth : This->numFaces;
            for (uint32_t faceSlice = 0; faceSlice < faceSlices; faceSlice++) {
                level_byte_length += slice->m_file_size;
                kimages[image].rgbSliceByteOffset = slice->m_file_ofs
                                               - level_file_offsets[level];
                kimages[image].rgbSliceByteLength = slice->m_file_size;
                if (bfh.m_flags & cBASISHeaderFlagHasAlphaSlices) {
                    slice++;
                    level_byte_length += slice->m_file_size;
                    kimages[image].alphaSliceByteOffset = slice->m_file_ofs
                                                  - level_file_offsets[level];
                    kimages[image].alphaSliceByteLength = slice->m_file_size;
                } else {
                    kimages[image].alphaSliceByteOffset = 0;
                    kimages[image].alphaSliceByteLength = 0;
                }
                // Get the IFrame flag, if it's set.
              kimages[image].imageFlags = slice->m_flags & ~cSliceDescFlagsHasAlpha;
                slice++;
                image++;
            }
        }
        priv._levelIndex[level].byteLength = level_byte_length;
        priv._levelIndex[level].uncompressedByteLength = 0;
        image_data_size += level_byte_length;
    }

    //
    // Copy the global code books & huffman tables to global data.
    //

    // Slightly sleazy but as image is now the last valid index in the
    // slice description array plus 1, so &kimages[image] points at the first
    // byte after where the endpoints, etc. must be written.
    uint8_t* dstptr = reinterpret_cast<uint8_t*>(&kimages[image]);
    // Copy the endpoints ...
    memcpy(dstptr,
           &bf[bfh.m_endpoint_cb_file_ofs],
           bfh.m_endpoint_cb_file_size);
    dstptr += bgdh.endpointsByteLength;
    // selectors ...
    memcpy(dstptr,
           &bf[bfh.m_selector_cb_file_ofs],
           bfh.m_selector_cb_file_size);
    dstptr += bgdh.selectorsByteLength;
    // and the huffman tables.
    memcpy(dstptr,
           &bf[bfh.m_tables_file_ofs],
           bfh.m_tables_file_size);

    assert((dstptr + bgdh.tablesByteLength - bgd) <= bgd_size);

    //
    // We have a complete global data package and compressed images.
    // Update This texture and copy compressed image data to it.
    //

    // Since we've left m_check_for_alpha set and m_force_alpha unset in
    // the compressor parameters, the basis encoder will not have included
    // an input alpha channel, if every alpha pixel in every image is 255.
    // This step occurs prior to encoding and supercompression and, per spec,
    // the DFD needs to reflect the input to the encoder not this texture.
    // Pass a parameter, set from the alpha flag of the emitted .basis header,
    // to rewriteDfd to allow it to do this.
    bool hasAlpha = (bfh.m_flags & cBASISHeaderFlagHasAlphaSlices) != 0;
    result = ktxTexture2_rewriteDfd(This, hasAlpha);
    if (result != KTX_SUCCESS) {
        delete bgd;
        return result;
    }

    uint8_t* new_data = (uint8_t*) malloc(image_data_size);
    if (!new_data)
        return KTX_OUT_OF_MEMORY;

    This->vkFormat = VK_FORMAT_UNDEFINED;
    This->supercompressionScheme = KTX_SUPERCOMPRESSION_BASIS;

    // Reflect this in the formatSize
    ktxFormatSize& formatSize = This->_protected->_formatSize;
    formatSize.flags = 0;
    formatSize.paletteSizeInBits = 0;
    formatSize.blockSizeInBits = 0 * 8;
    formatSize.blockWidth = 1;
    formatSize.blockHeight = 1;
    formatSize.blockDepth = 1;
    // and the requiredLevelAlignment.
    This->_private->_requiredLevelAlignment = 1;

    // Since we only allow 8-bit components to be compressed ...
    assert(This->_protected->_typeSize == 1);

    priv._supercompressionGlobalData = bgd;
    priv._sgdByteLength = bgd_size;


    This->pData = new_data;
    This->dataSize = image_data_size;

    // Copy in the compressed image data.
    // NOTA BENE: Mipmap levels are ordered from largest to smallest in .basis.
    // We have to reorder.

    uint64_t level_offset = 0;
    for (int32_t level = This->numLevels - 1; level >= 0; level--) {
        priv._levelIndex[level].byteOffset = level_offset;
        // byteLength was set in loop above
        memcpy(This->pData + level_offset,
               &bf[level_file_offsets[level]],
               priv._levelIndex[level].byteLength);
        level_offset += priv._levelIndex[level].byteLength;
    }

    return KTX_SUCCESS;
}

/**
 * @memberof ktxTexture2
 * @ingroup writer
 * @~English
 * @brief Supercompress a KTX2 texture with uncpompressed images.
 *
 * The images are encoded to ETC1S block-compressed format and supercompressed
 * with Basis Universal. The encoded images replace the original images and the
 * texture's fields including the DFD are modified to reflect the new state.
 *
 * Such textures must be transcoded to a desired target block compressed format
 * before they can be uploaded to a GPU via a graphics API.
 *
 * @sa ktxTexture2_CompressBasisEx().
 *
 * @param[in]   This    pointer to the ktxTexture2 object of interest.
 * @param[in]   quality Compression quality, a value from 1 - 255. Default is
                        128 which is selected if @p quality is 0. Lower=better
                        compression/lower quality/faster. Higher=less
                        compression/higher quality/slower.
 *
 * @return      KTX_SUCCESS on success, other KTX_* enum values on error.
 *
 * @exception KTX_INVALID_OPERATION
 *                              The texture is already supercompressed.
 * @exception KTX_INVALID_OPERATION
 *                              The texture's image are in a block compressed
 *                              format.
 * @exception KTX_INVALID_OPERATION
 *                              The texture's images are 1D. Only 2D images can
 *                              be supercompressed.
 * @exception KTX_OUT_OF_MEMORY Not enough memory to carry out supercompression.
 */
extern "C" KTX_error_code
ktxTexture2_CompressBasis(ktxTexture2* This, ktx_uint32_t quality)
{
    ktxBasisParams params = {};
    params.structSize = sizeof(params);
    params.threadCount = 1;
    params.qualityLevel = quality;

    return ktxTexture2_CompressBasisEx(This, &params);
}
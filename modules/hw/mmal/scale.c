/*****************************************************************************
 * scale.c:
 *   Video filter which scales images using the DispmanX hardware rendering
 *   capabilities
 *****************************************************************************
 * Copyright © 2015 jusst technologies GmbH
 *
 * Authors: Julian Scheel <julian@jusst.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>

#include <bcm_host.h>

static int OpenFilter (vlc_object_t *);
static picture_t *Filter (filter_t *, picture_t *);

vlc_module_begin ()
    set_description(N_("DispmanX video scaling filter"))
    set_capability("video filter2", 200)
    set_callbacks(OpenFilter, NULL)
vlc_module_end ()

struct filter_sys_t {
};

static int OpenFilter(vlc_object_t *p_this)
{
    filter_t *filter = (filter_t*)p_this;

    if ((filter->fmt_in.video.i_chroma != VLC_CODEC_YUVP &&
          filter->fmt_in.video.i_chroma != VLC_CODEC_YUVA &&
          filter->fmt_in.video.i_chroma != VLC_CODEC_RGB32 &&
          filter->fmt_in.video.i_chroma != VLC_CODEC_RGBA) ||
        (filter->fmt_out.video.i_chroma != VLC_CODEC_RGBA)) {
        return VLC_EGENERIC;
    }

    if (filter->fmt_in.video.orientation != filter->fmt_out.video.orientation)
        return VLC_EGENERIC;

    bcm_host_init();
    filter->pf_video_filter = Filter;

    msg_Dbg(filter, "%ix%i -> %ix%i", filter->fmt_in.video.i_width,
             filter->fmt_in.video.i_height, filter->fmt_out.video.i_width,
             filter->fmt_out.video.i_height);

    return VLC_SUCCESS;
}

#define DPM_RGBA32(a, r, g, b) (((uint8_t)(a)) << 24 | ((uint8_t)(r)) << 16 | ((uint8_t)(g)) << 8 | ((uint8_t)(b)) << 0)
static picture_t *Filter(filter_t *filter, picture_t *pic)
{
    video_format_t *fmt_out = &filter->fmt_out.video;
    video_format_t *fmt_in = &filter->fmt_in.video;
    DISPMANX_RESOURCE_HANDLE_T dest_res;
    DISPMANX_RESOURCE_HANDLE_T src_res;
    DISPMANX_DISPLAY_HANDLE_T display;
    DISPMANX_ELEMENT_HANDLE_T src_el;
    static unsigned int palette[256];
    DISPMANX_UPDATE_HANDLE_T update;
    uint32_t dest_image_handle;
    VC_DISPMANX_ALPHA_T alpha;
    uint32_t src_image_handle;
    VC_IMAGE_TYPE_T img_type;
    VC_RECT_T bmp_rect;
    VC_RECT_T src_rect;
    VC_RECT_T dst_rect;
    int src_height;
    int dst_height;
    int src_width;
    int dst_width;
    int src_pitch;
    int dst_pitch;
    int i;

    picture_t *pic_dst;

    if (!pic)
        return NULL;

    if ((fmt_in->i_height == 0) ||
        (fmt_in->i_width == 0))
        return NULL;

    if ((fmt_out->i_height == 0) ||
        (fmt_out->i_width == 0))
        return NULL;

    video_format_ScaleCropAr(fmt_out, fmt_in);

    /* Request output picture */
    pic_dst = filter_NewPicture(filter);
    if (!pic_dst) {
        picture_Release(pic);
        return NULL;
    }

    dest_res = vc_dispmanx_resource_create(VC_IMAGE_RGBA32,
            fmt_out->i_width, fmt_out->i_height, &dest_image_handle);

    display = vc_dispmanx_display_open_offscreen(
            dest_res, DISPMANX_NO_ROTATE);

    if (fmt_in->i_chroma == VLC_CODEC_YUVP)
        img_type = VC_IMAGE_8BPP;
    else if (fmt_in->i_chroma == VLC_CODEC_YUVA) {
        msg_Dbg(filter, "YUVA not supported yet...");
        img_type = VC_IMAGE_YUV420;
    } else
        img_type = VC_IMAGE_RGBA32;

    src_pitch = pic->p->i_pitch;
    dst_pitch = pic_dst->p->i_pitch;
    src_height = filter->fmt_in.video.i_height;
    src_width = filter->fmt_in.video.i_width;
    dst_height = filter->fmt_out.video.i_height;
    dst_width = filter->fmt_out.video.i_width;

    vc_dispmanx_rect_set(&bmp_rect, 0, 0, src_width, src_height);
    vc_dispmanx_rect_set(&src_rect, 0, 0, src_width << 16, src_height << 16);
    vc_dispmanx_rect_set(&dst_rect, 0, 0, dst_width, dst_height);

    update = vc_dispmanx_update_start(0);

    /* upload src image to dispmanx */
    src_res = vc_dispmanx_resource_create(img_type,
            src_width | (src_pitch << 16), src_height | (src_height << 16),
            &src_image_handle);
    vc_dispmanx_resource_write_data(src_res, img_type, src_pitch,
            pic->p[0].p_pixels, &bmp_rect);

    /* Convert YUV palette to RGB palette and push to dispmanx */
    if (fmt_in->i_chroma == VLC_CODEC_YUVP) {
        uint8_t *yuv_pal;
        for (i = 0; i < fmt_in->p_palette->i_entries; i++) {
            yuv_pal = fmt_in->p_palette->palette[i];
            palette[i] = DPM_RGBA32(yuv_pal[3],
                                   1.164 * (yuv_pal[0] - 16) + 2.018 * (yuv_pal[1] - 128),
                                   1.164 * (yuv_pal[0] - 16) - 0.813 * (yuv_pal[2] - 128) - 0.391 * (yuv_pal[1] - 128),
                                   1.163 * (yuv_pal[0] - 16) + 1.596 * (yuv_pal[2] - 128));
        }
        vc_dispmanx_resource_set_palette(src_res, palette, 0, sizeof(palette));
    }

    alpha.mask = DISPMANX_NO_HANDLE;
    alpha.flags = DISPMANX_FLAGS_ALPHA_FROM_SOURCE | DISPMANX_FLAGS_ALPHA_MIX;
    alpha.opacity = 255;

    /* place image to scaled output */
    src_el = vc_dispmanx_element_add(update, display, 0, &dst_rect, src_res,
            &src_rect, DISPMANX_PROTECTION_NONE, &alpha, NULL, VC_IMAGE_ROT0);

    /* execute the scaling */
    vc_dispmanx_update_submit_sync(update);

    /* read scaled image back */
    vc_dispmanx_resource_read_data(dest_res, &dst_rect,
            pic_dst->p[0].p_pixels, dst_pitch);

    /* teardown */
    vc_dispmanx_element_remove(update, src_el);
    vc_dispmanx_display_close(display);
    vc_dispmanx_resource_delete(src_res);
    vc_dispmanx_resource_delete(dest_res);

    picture_CopyProperties(pic_dst, pic);
    picture_Release(pic);
    return pic_dst;
}


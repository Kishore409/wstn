/*
 * Copyright © 2019 Harish Krupo
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>

#include "va_renderer.h"
#include "compositor.h"
#include "linux-dmabuf.h"
#include "shared/helpers.h"

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010         fourcc_code('P', '0', '1', '0') /* 2x2 subsampled Cb:Cr plane 10 bits per channel */
#endif

#define CHECK_VASTATUS(va_status,func)                                      \
	if (va_status != VA_STATUS_SUCCESS) {                                     \
		weston_log("%s:%s (%d) failed\n", __func__, func, __LINE__);         \
	}

struct va_renderer {
	int gpu_fd;
	void* va_display;
	VAContextID va_context;
	VAConfigID va_config;
	uint32_t render_target_format;
	struct gbm_device *gbm;
};

struct va_renderer *
va_renderer_initialize(int gpu_fd, struct gbm_device *gbm)
{
	struct va_renderer *r = NULL;
	void *va_display = NULL;

	r = zalloc(sizeof *r);
	if (r == NULL)
		return NULL;

	va_display = vaGetDisplayDRM(gpu_fd);
	if (!va_display) {
		weston_log("vaGetDisplay failed\n");
		goto err;
	}

	r->va_display = va_display;
	r->gbm = gbm;

	VAStatus ret = VA_STATUS_SUCCESS;
	int major, minor;
	ret = vaInitialize(r->va_display, &major, &minor);
	if (ret == VA_STATUS_SUCCESS)
		return r;

err:
	free(r);
	return NULL;
}

static void
va_renderer_destroy_context(struct va_renderer *r)
{
	if (r->va_context != VA_INVALID_ID) {
		vaDestroyContext(r->va_display, r->va_context);
		r->va_context = VA_INVALID_ID;
	}
	if (r->va_config != VA_INVALID_ID) {
		vaDestroyConfig(r->va_display, r->va_config);
		r->va_config = VA_INVALID_ID;
	}

}

static int
va_drm_format_to_rt_format(int format) {
	switch (format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVYU:
	case DRM_FORMAT_VYUY:
		return VA_RT_FORMAT_YUV420;
	case DRM_FORMAT_YUV422:
		return VA_RT_FORMAT_YUV422;
	case DRM_FORMAT_YUV444:
		return VA_RT_FORMAT_YUV444;
	case DRM_FORMAT_P010:
		return VA_RT_FORMAT_YUV420;
	default:
		weston_log("Unknown drm format\n");
		break;
	}
	return 0;
}

static bool
va_renderer_create_context(struct va_renderer *r)
{
	va_renderer_destroy_context(r);

	// the render_target_format is assumed to be set before calling create_context
	VAConfigAttrib config_attrib;
	config_attrib.type = VAConfigAttribRTFormat;
	config_attrib.value = r->render_target_format;
	VAStatus ret =
		vaCreateConfig(r->va_display, VAProfileNone, VAEntrypointVideoProc,
			       &config_attrib, 1, &r->va_config);
	if (ret != VA_STATUS_SUCCESS) {
		return false;
	}

	// These parameters are not used in vaCreateContext so just set them to dummy
	// values
	int width = 1;
	int height = 1;
	ret = vaCreateContext(r->va_display, r->va_config, width, height,
			      0x00, NULL, 0, &r->va_context);

	return ret == VA_STATUS_SUCCESS ? true : false;
}

static struct gbm_bo *
va_get_bo_from_view(struct va_renderer *r, struct weston_view *v)
{
	struct linux_dmabuf_buffer *dmabuf;
	struct weston_buffer *buffer = v->surface->buffer_ref.buffer;
	struct gbm_bo *bo = NULL;

	if (!buffer)
		return NULL;

	dmabuf = linux_dmabuf_buffer_get(buffer->resource);
	if (dmabuf) {
#ifdef HAVE_GBM_FD_IMPORT
		struct gbm_import_fd_data import_legacy = {
			.width = dmabuf->attributes.width,
			.height = dmabuf->attributes.height,
			.format = dmabuf->attributes.format,
			.stride = dmabuf->attributes.stride[0],
			.fd = dmabuf->attributes.fd[0],
		};
		struct gbm_import_fd_modifier_data import_mod = {
			.width = dmabuf->attributes.width,
			.height = dmabuf->attributes.height,
			.format = dmabuf->attributes.format,
			.num_fds = dmabuf->attributes.n_planes,
			.modifier = dmabuf->attributes.modifier[0],
		};

		/* XXX: TODO:
		 *
		 * Currently the buffer is rejected if any dmabuf attribute
		 * flag is set.  This keeps us from passing an inverted /
		 * interlaced / bottom-first buffer (or any other type that may
		 * be added in the future) through to an overlay.  Ultimately,
		 * these types of buffers should be handled through buffer
		 * transforms and not as spot-checks requiring specific
		 * knowledge. */
		if (dmabuf->attributes.flags)
			return NULL;

		static_assert(ARRAY_LENGTH(import_mod.fds) ==
			      ARRAY_LENGTH(dmabuf->attributes.fd),
			      "GBM and linux_dmabuf FD size must match");
		static_assert(sizeof(import_mod.fds) == sizeof(dmabuf->attributes.fd),
			      "GBM and linux_dmabuf FD size must match");
		memcpy(import_mod.fds, dmabuf->attributes.fd, sizeof(import_mod.fds));

		static_assert(ARRAY_LENGTH(import_mod.strides) ==
			      ARRAY_LENGTH(dmabuf->attributes.stride),
			      "GBM and linux_dmabuf stride size must match");
		static_assert(sizeof(import_mod.strides) ==
			      sizeof(dmabuf->attributes.stride),
			      "GBM and linux_dmabuf stride size must match");
		memcpy(import_mod.strides, dmabuf->attributes.stride,
		       sizeof(import_mod.strides));

		static_assert(ARRAY_LENGTH(import_mod.offsets) ==
			      ARRAY_LENGTH(dmabuf->attributes.offset),
			      "GBM and linux_dmabuf offset size must match");
		static_assert(sizeof(import_mod.offsets) ==
			      sizeof(dmabuf->attributes.offset),
			      "GBM and linux_dmabuf offset size must match");
		memcpy(import_mod.offsets, dmabuf->attributes.offset,
		       sizeof(import_mod.offsets));

		/* The legacy FD-import path does not allow us to supply modifiers,
		 * multiple planes, or buffer offsets. */
		if (dmabuf->attributes.modifier[0] != DRM_FORMAT_MOD_INVALID ||
		    import_mod.num_fds > 1 ||
		    import_mod.offsets[0] > 0) {
			bo = gbm_bo_import(r->gbm, GBM_BO_IMPORT_FD_MODIFIER,
					   &import_mod,
					   GBM_BO_USE_SCANOUT);
		} else {
			bo = gbm_bo_import(r->gbm, GBM_BO_IMPORT_FD,
					   &import_legacy,
					   GBM_BO_USE_SCANOUT);
		}
#endif
	} else {

		bo = gbm_bo_import(r->gbm, GBM_BO_IMPORT_WL_BUFFER,
				   buffer->resource, GBM_BO_USE_SCANOUT);
		if (!bo)
			return NULL;
	}

	return bo;
}

static int
va_drm_format_to_va_format(int format) {
	switch (format) {
	case DRM_FORMAT_NV12:
		return VA_FOURCC_NV12;
	case DRM_FORMAT_YVU420:
		return VA_FOURCC_YV12;
	case DRM_FORMAT_YUV420:
		return VA_FOURCC('I', '4', '2', '0');
	case DRM_FORMAT_YUV422:
		return VA_FOURCC_YUY2;
	case DRM_FORMAT_UYVY:
		return VA_FOURCC_UYVY;
	case DRM_FORMAT_YUYV:
		return VA_FOURCC_YUY2;
	case DRM_FORMAT_P010:
		return VA_FOURCC_P010;
	case DRM_FORMAT_YVYU:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_YUV444:
	case DRM_FORMAT_AYUV:
	default:
		weston_log("Unable to convert to VAFormat from format %x", format);
		break;
	}
	return 0;
}

static VASurfaceID
va_get_surface_from_gbm_bo(struct va_renderer *r,
			   struct gbm_bo *bo)
{
	int width, height, fd, total_planes;
	VASurfaceID surface;
	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	fd = gbm_bo_get_fd(bo);
	total_planes = gbm_bo_get_plane_count(bo);

	VASurfaceAttribExternalBuffers external;
	memset(&external, 0, sizeof(external));
	uint32_t rt_format = va_drm_format_to_rt_format(DRM_FORMAT_P010);
	external.pixel_format = va_drm_format_to_va_format(DRM_FORMAT_P010);
	external.width = width;
	external.height = height;
	external.num_planes = total_planes;
#if VA_MAJOR_VERSION < 1
	unsigned long prime_fds[total_planes];
#else
	uintptr_t prime_fds[total_planes];
#endif
	// Here the assumption is that the same fd is used for all the planes
	for (int i = 0; i < total_planes; i++) {
		external.pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
		external.offsets[i] = gbm_bo_get_offset(bo, i);
		prime_fds[i] = fd;
	}

	external.num_buffers = total_planes;
	external.buffers = prime_fds;

	VASurfaceAttrib attribs[2];
	attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[0].type = VASurfaceAttribMemoryType;
	attribs[0].value.type = VAGenericValueTypeInteger;
	attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;

	attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
	attribs[1].value.type = VAGenericValueTypePointer;
	attribs[1].value.value.p = &external;

	VAStatus ret =
		vaCreateSurfaces(r->va_display, rt_format, width, height,
				 &surface, 1, attribs, 2);
	if (ret != VA_STATUS_SUCCESS)
		weston_log("Failed to create VASurface from drmbuffer with ret %x", ret);

	// close gbm_bo fd here?
	return surface;

}

bool
va_renderer_tonemap(struct va_renderer *r,
		    struct weston_view *v)
{
	struct gbm_bo *bo = NULL;
	struct weston_surface *surface = v->surface;
	int width, height;
	VASurfaceID src_surface, dst_surface;
	VAProcFilterParameterBufferHDRToneMapping tone_mapping_filter;
	VAHdrMetaData va_hdr_metadata;
	VAHdrMetaDataHDR10 va_sm;
	struct weston_hdr_metadata_static *sm;
	VABufferID hdr_filter_buffer, pipeline_buffer;
	VAStatus status;

	uint32_t rt_format = va_drm_format_to_rt_format(DRM_FORMAT_P010);
	uint32_t va_format = va_drm_format_to_va_format(DRM_FORMAT_P010);

	if (!surface->hdr_metadata)
		return false;

	if (r->va_context == VA_INVALID_ID ||
	    r->render_target_format != rt_format) {
		r->render_target_format = rt_format;
		if (!va_renderer_create_context(r)) {
			weston_log("Create VA context failed\n");
			return false;
		}
	}

	/*Query Filter's Caps: The return value will be HDR10 and H2S, H2H, H2E. */
	VAProcFilterCapHighDynamicRange hdrtm_caps[VAProcHighDynamicRangeMetadataTypeCount];
	uint32_t num_hdrtm_caps = VAProcHighDynamicRangeMetadataTypeCount;
	memset(&hdrtm_caps, 0, sizeof(VAProcFilterCapHighDynamicRange)*num_hdrtm_caps);
	status = vaQueryVideoProcFilterCaps(r->va_display, r->va_context,
			VAProcFilterHighDynamicRangeToneMapping,
			(void *)hdrtm_caps, &num_hdrtm_caps);
	CHECK_VASTATUS(status,"vaQueryVideoProcFilterCaps");
	weston_log("vaQueryVideoProcFilterCaps num_hdrtm_caps %d\n", num_hdrtm_caps);
	for (uint32_t i = 0; i < num_hdrtm_caps; ++i)    {
		weston_log("vaQueryVideoProcFilterCaps hdrtm_caps[%d]: metadata type %d, flag %d\n", i, hdrtm_caps[i].metadata_type, hdrtm_caps[i].caps_flag);
	}

	bo = va_get_bo_from_view(r, v);
	if (!bo)
		return false;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);

	src_surface = va_get_surface_from_gbm_bo(r, bo);

	VASurfaceAttrib attrib;
	attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
	attrib.type = VASurfaceAttribPixelFormat;
	attrib.value.type = VAGenericValueTypeInteger;
	attrib.value.value.i = va_format;
	status = vaCreateSurfaces(r->va_display,
					   rt_format, width, height,
					   &dst_surface, 1, &attrib, 1);

	if (status != VA_STATUS_SUCCESS) {
		weston_log("Unable to create intermediate surface\n");
		return false;
	}

	VARectangle surface_region;
	surface_region.x = 0;
	surface_region.y = 0;
	surface_region.width = width;
	surface_region.height = height;

	sm = &surface->hdr_metadata->metadata.static_metadata;
	va_sm.display_primaries_x[0] = sm->display_primary_g_x;
	va_sm.display_primaries_x[1] = sm->display_primary_b_x;
	va_sm.display_primaries_x[2] = sm->display_primary_r_x;

	va_sm.display_primaries_y[0] = sm->display_primary_g_y;
	va_sm.display_primaries_y[1] = sm->display_primary_b_y;
	va_sm.display_primaries_y[2] = sm->display_primary_r_y;

	va_sm.white_point_x = sm->white_point_x;
	va_sm.white_point_y = sm->white_point_y;

	va_sm.max_display_mastering_luminance = sm->max_luminance;
	va_sm.min_display_mastering_luminance = sm->min_luminance;

	va_sm.max_content_light_level = sm->max_cll;
	va_sm.max_pic_average_light_level = sm->max_fall;

	va_hdr_metadata.metadata_type = VAProcHighDynamicRangeMetadataHDR10;
	va_hdr_metadata.metadata = &va_sm;
	va_hdr_metadata.metadata_size = sizeof(VAHdrMetaDataHDR10);

	tone_mapping_filter.type = VAProcFilterHighDynamicRangeToneMapping;
	tone_mapping_filter.data = va_hdr_metadata;

	vaCreateBuffer(r->va_display, r->va_context, VAProcFilterParameterBufferType,
		       sizeof(VAProcFilterParameterBufferHDRToneMapping),
		       1, &tone_mapping_filter, &hdr_filter_buffer);

	VAProcPipelineParameterBuffer pipe_param = {};
	pipe_param.surface = src_surface;
	pipe_param.surface_region = &surface_region;
	pipe_param.surface_color_standard = VAProcColorStandardBT2020;
	pipe_param.output_region = &surface_region;
	pipe_param.output_color_standard = VAProcColorStandardBT2020;
	pipe_param.filters = &hdr_filter_buffer;
	pipe_param.num_filters = 1;
	pipe_param.output_hdr_metadata = NULL;

	vaCreateBuffer(r->va_display, r->va_context, VAProcPipelineParameterBufferType,
		       sizeof(VAProcPipelineParameterBuffer),
		       1, &pipe_param, &pipeline_buffer);


	VAStatus ret = VA_STATUS_SUCCESS;
	ret = vaBeginPicture(r->va_display, r->va_context, dst_surface);
	ret |=
		vaRenderPicture(r->va_display, r->va_context, &pipeline_buffer, 1);
	ret |= vaEndPicture(r->va_display, r->va_context);

	vaDestroyBuffer(r->va_display, pipeline_buffer);
	vaDestroyBuffer(r->va_display, hdr_filter_buffer);

	memset(&pipe_param, 0, sizeof(VAProcPipelineParameterBuffer));

	pipe_param.surface = dst_surface;
	pipe_param.surface_region = &surface_region;
	pipe_param.surface_color_standard = VAProcColorStandardBT2020;
	pipe_param.output_region = &surface_region;
	pipe_param.output_color_standard = VAProcColorStandardBT2020;

	vaCreateBuffer(r->va_display, r->va_context, VAProcPipelineParameterBufferType,
		       sizeof(VAProcPipelineParameterBuffer),
		       1, &pipe_param, &pipeline_buffer);


	ret = VA_STATUS_SUCCESS;
	ret = vaBeginPicture(r->va_display, r->va_context, src_surface);
	ret |=
		vaRenderPicture(r->va_display, r->va_context, &pipeline_buffer, 1);
	ret |= vaEndPicture(r->va_display, r->va_context);

	vaDestroyBuffer(r->va_display, pipeline_buffer);

	/* vaDestroySurfaces(r->va_display, &src_surface, 1); */
	/* vaDestroySurfaces(r->va_display, &dst_surface, 1); */

	return true;
}

void va_renderer_fini(struct va_renderer *r)
{
	va_renderer_destroy_context(r);
	vaTerminate(r->va_display);
}


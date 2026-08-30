/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / HEVC video decoder filter
 *  based on libde265 (https://github.com/strukturag/libde265)
 *
 *  Pairs with a container demuxer (e.g. isobmff) that provides framed,
 *  length-prefixed HEVC access units with an out-of-band HVCC decoder
 *  config (GF_PROP_PID_DECODER_CONFIG) - this filter does not parse
 *  Annex-B start codes itself.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <gpac/mpeg4_odf.h>
#include <string.h>
#include <stdio.h>

#include <libde265/de265.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;

	de265_decoder_context *decoder;
	Bool decoder_ready;

	u32 nalu_size_length;
	u32 width, height;
	Bool is_playing;
} GF_HEVCDecCtx;

static GF_Err hevcdec_push_param_sets(GF_HEVCDecCtx *ctx, const GF_PropertyValue *dsi)
{
	GF_HEVCConfig *cfg;
	u32 i, j;
	de265_error err;

	cfg = gf_odf_hevc_cfg_read(dsi->value.data.ptr, dsi->value.data.size, GF_FALSE);
	if (!cfg) return GF_NON_COMPLIANT_BITSTREAM;

	ctx->nalu_size_length = cfg->nal_unit_size;

	for (i = 0; i < gf_list_count(cfg->param_array); i++)
	{
		GF_NALUFFParamArray *ar = (GF_NALUFFParamArray *)gf_list_get(cfg->param_array, i);
		for (j = 0; j < gf_list_count(ar->nalus); j++)
		{
			GF_NALUFFParam *sl = (GF_NALUFFParam *)gf_list_get(ar->nalus, j);
			err = de265_push_NAL(ctx->decoder, sl->data, sl->size, 0, NULL);
			if (!de265_isOK(err))
			{
				GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[HEVCDec] Failed to push param set NAL: %s\n", de265_get_error_text(err)));
			}
		}
	}
	gf_odf_hevc_cfg_del(cfg);
	return GF_OK;
}

static GF_Err hevcdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *dsi;
	GF_HEVCDecCtx *ctx = (GF_HEVCDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		if (ctx->decoder)
		{
			de265_free_decoder(ctx->decoder);
			ctx->decoder = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}

	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YUV));

	if (!ctx->decoder)
	{
		ctx->decoder = de265_new_decoder();
		if (!ctx->decoder) return GF_OUT_OF_MEM;
	}

	dsi = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG);
	if (dsi && dsi->value.data.size)
	{
		GF_Err e = hevcdec_push_param_sets(ctx, dsi);
		if (e) return e;
	}

	return GF_OK;
}

static Bool hevcdec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_HEVCDecCtx *ctx = (GF_HEVCDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static void hevcdec_send_frame(GF_HEVCDecCtx *ctx, const de265_image *img)
{
	GF_FilterPacket *dst_pck;
	u8 *output;
	u32 y, c;
	u32 w = de265_get_image_width(img, 0);
	u32 h = de265_get_image_height(img, 0);
	u32 cw = de265_get_image_width(img, 1);
	u32 ch = de265_get_image_height(img, 1);
	u32 y_size = w * h;
	u32 c_size = cw * ch;
	u32 out_size = y_size + 2 * c_size;
	s32 stride;
	const u8 *plane;

	if ((w != ctx->width) || (h != ctx->height))
	{
		ctx->width = w;
		ctx->height = h;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(w));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(h));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(w));
	}

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
	if (!dst_pck) return;

	plane = de265_get_image_plane(img, 0, &stride);
	for (y = 0; y < h; y++)
	{
		memcpy(output + y * w, plane + y * stride, w);
	}
	output += y_size;

	for (c = 1; c <= 2; c++)
	{
		plane = de265_get_image_plane(img, c, &stride);
		for (y = 0; y < ch; y++)
		{
			memcpy(output + y * cw, plane + y * stride, cw);
		}
		output += c_size;
	}

	gf_filter_pck_set_cts(dst_pck, (u64)de265_get_image_PTS(img));
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);
}

static void hevcdec_flush_pictures(GF_HEVCDecCtx *ctx)
{
	const de265_image *img;
	while ((img = de265_get_next_picture(ctx->decoder)) != NULL)
	{
		hevcdec_send_frame(ctx, img);
	}
}

static GF_Err hevcdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	u8 *data;
	u32 size, pos;
	u64 cts;
	de265_error err;
	int more;
	GF_HEVCDecCtx *ctx = (GF_HEVCDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			de265_flush_data(ctx->decoder);
			more = 1;
			while (more)
			{
				err = de265_decode(ctx->decoder, &more);
				if (!de265_isOK(err)) break;
				hevcdec_flush_pictures(ctx);
			}
			hevcdec_flush_pictures(ctx);
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}
	cts = gf_filter_pck_get_cts(pck);

	pos = 0;
	while (pos + ctx->nalu_size_length <= size)
	{
		u32 i, nal_len = 0;
		for (i = 0; i < ctx->nalu_size_length; i++)
		{
			nal_len = (nal_len << 8) | data[pos + i];
		}
		pos += ctx->nalu_size_length;
		if (pos + nal_len > size) break;

		err = de265_push_NAL(ctx->decoder, data + pos, nal_len, (de265_PTS)cts, NULL);
		if (!de265_isOK(err))
		{
			GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[HEVCDec] Failed to push NAL: %s\n", de265_get_error_text(err)));
		}
		pos += nal_len;
	}

	gf_filter_pid_drop_packet(ctx->ipid);

	more = 1;
	while (more)
	{
		err = de265_decode(ctx->decoder, &more);
		if (err == DE265_ERROR_WAITING_FOR_INPUT_DATA)
		{
			more = 0;
			break;
		}
		if (!de265_isOK(err)) break;
		hevcdec_flush_pictures(ctx);
	}
	hevcdec_flush_pictures(ctx);

	return GF_OK;
}

static void hevcdec_finalize(GF_Filter *filter)
{
	GF_HEVCDecCtx *ctx = (GF_HEVCDecCtx *)gf_filter_get_udta(filter);
	if (ctx->decoder)
	{
		de265_free_decoder(ctx->decoder);
		ctx->decoder = NULL;
	}
}

static const GF_FilterCapability HEVCDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_HEVC),
		CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister HEVCDecoderRegister = {
	.name = "hevcdec265",
	GF_FS_SET_DESCRIPTION("HEVC video decoder")
		GF_FS_SET_HELP("This filter decodes HEVC video elementary streams using libde265.")
			.private_size = sizeof(GF_HEVCDecCtx),
	SETCAPS(HEVCDecCaps),
	.configure_pid = hevcdec_configure_pid,
	.process = hevcdec_process,
	.process_event = hevcdec_process_event,
	.finalize = hevcdec_finalize,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE dynCall_hevcdec265_register(GF_FilterSession *session)
{
	return &HEVCDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_hevcdec265(void) {
    gf_filter_auto_register("hevcdec265", dynCall_hevcdec265_register);
}

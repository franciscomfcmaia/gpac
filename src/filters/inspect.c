/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2017-2020
 *					All rights reserved
 *
 *  This file is part of GPAC / inspection filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <gpac/list.h>
#include <gpac/xml.h>
#include <gpac/internal/media_dev.h>

typedef struct
{
	GF_FilterPid *src_pid;
	FILE *tmp;
	u64 pck_num;
	u32 idx;
	u8 dump_pid; //0: no, 1: configure/reconfig, 2: info update
	u8 init_pid_config_done;
	u64 pck_for_config;
	u64 prev_dts, prev_cts, init_ts;
	u32 codec_id;

#ifndef GPAC_DISABLE_AV_PARSERS
	HEVCState *hevc_state;
	AVCState *avc_state;
	AV1State *av1_state;
	GF_M4VParser *mv124_state;
	GF_M4VDecSpecInfo dsi;
#endif

	Bool has_svcc;
	u32 nalu_size_length;
	Bool is_adobe_protected;
	Bool is_cenc_protected;

	GF_Fraction tmcd_rate;
	u32 tmcd_flags;
	u32 tmcd_fpt;
} PidCtx;

enum
{
	INSPECT_MODE_PCK=0,
	INSPECT_MODE_BLOCK,
	INSPECT_MODE_REFRAME,
	INSPECT_MODE_RAW
};

enum
{
	INSPECT_TEST_NO=0,
	INSPECT_TEST_NOPROP,
	INSPECT_TEST_NETWORK,
	INSPECT_TEST_ENCODE,
};

typedef struct
{
	u32 mode;
	Bool interleave;
	Bool dump_data;
	Bool deep;
	char *log;
	char *fmt;
	Bool props, hdr, allp, info, pcr, analyze, xml;
	Double speed, start;
	u32 test;
	GF_Fraction dur;
	Bool dump_crc, dtype;
	Bool fftmcd;

	FILE *dump;

	GF_List *src_pids;

	Bool is_prober, probe_done, hdr_done, dump_pck;
} GF_InspectCtx;

#define DUMP_ATT_STR(_name, _val) if (ctx->xml) { \
		gf_fprintf(dump, " %s=\"%s\"", _name, _val); \
	} else { \
		gf_fprintf(dump, " %s %s", _name, _val); \
	}

#define DUMP_ATT_LLU(_name, _val) if (ctx->xml) { \
		gf_fprintf(dump, " %s=\""LLU"\"", _name, _val);\
	} else {\
		gf_fprintf(dump, " %s "LLU, _name, _val);\
	}

#define DUMP_ATT_U(_name, _val) if (ctx->xml) { \
		gf_fprintf(dump, " %s=\"%u\"", _name, _val);\
	} else {\
		gf_fprintf(dump, " %s %u", _name, _val);\
	}

#define DUMP_ATT_D(_name, _val) if (ctx->xml) { \
		gf_fprintf(dump, " %s=\"%d\"", _name, _val);\
	} else {\
		gf_fprintf(dump, " %s %d", _name, _val);\
	}

#define DUMP_ATT_X(_name, _val)  if (ctx->xml) { \
		gf_fprintf(dump, " %s=\"0x%08X\"", _name, _val);\
	} else {\
		gf_fprintf(dump, " %s 0x%08X", _name, _val);\
	}



#ifndef GPAC_DISABLE_AV_PARSERS
static u32 inspect_get_nal_size(char *ptr, u32 nalh_size)
{
	u32 nal_size=0;
	u32 v = nalh_size;
	while (v) {
		nal_size |= (u8) *ptr;
		ptr++;
		v-=1;
		if (v) nal_size <<= 8;
	}
	return nal_size;
}

static void dump_sei(FILE *dump, GF_BitStream *bs, Bool is_hevc)
{
	u32 sei_idx=0;
	u32 i;
	gf_bs_enable_emulation_byte_removal(bs, GF_TRUE);

	//skip nal header
	gf_bs_read_int(bs, is_hevc ? 16 : 8);

	gf_fprintf(dump, " SEI=\"");
	while (gf_bs_available(bs) ) {
		u32 sei_type = 0;
		u32 sei_size = 0;
		while (gf_bs_peek_bits(bs, 8, 0) == 0xFF) {
			sei_type += 255;
			gf_bs_read_int(bs, 8);
		}
		sei_type += gf_bs_read_int(bs, 8);
		while (gf_bs_peek_bits(bs, 8, 0) == 0xFF) {
			sei_size += 255;
			gf_bs_read_int(bs, 8);
		}
		sei_size += gf_bs_read_int(bs, 8);
		i=0;
		while (i < sei_size) {
			gf_bs_read_u8(bs);
			i++;
		}
		if (sei_idx) gf_fprintf(dump, ",");
		gf_fprintf(dump, "(type=%u, size=%u)", sei_type, sei_size);
		sei_idx++;
		if (gf_bs_peek_bits(bs, 8, 0) == 0x80) {
			break;
		}
	}
	gf_fprintf(dump, "\"");
}

GF_EXPORT
void gf_inspect_dump_nalu(FILE *dump, u8 *ptr, u32 ptr_size, Bool is_svc, HEVCState *hevc, AVCState *avc, u32 nalh_size, Bool dump_crc, Bool is_encrypted)
{
	s32 res = 0;
	u8 type, nal_ref_idc;
	u8 dependency_id, quality_id, temporal_id;
	u8 track_ref_index;
	s8 sample_offset;
	u32 data_offset, data_size;
	s32 idx;
	GF_BitStream *bs;

	if (!ptr_size) {
		gf_fprintf(dump, "error=\"invalid nal size 0\"");
		return;
	}

	if (dump_crc) gf_fprintf(dump, "crc=\"%u\" ", gf_crc_32(ptr, ptr_size) );

	if (hevc) {
#ifndef GPAC_DISABLE_HEVC
		if (ptr_size==1) {
			gf_fprintf(dump, "error=\"invalid nal size 1\"");
			return;
		}
		res = gf_media_hevc_parse_nalu(ptr, ptr_size, hevc, &type, &temporal_id, &quality_id);

		gf_fprintf(dump, "code=\"%d\" type=\"", type);

		switch (type) {
		case GF_HEVC_NALU_SLICE_TRAIL_N:
			gf_fputs("TRAIL_N slice segment", dump);
			break;
		case GF_HEVC_NALU_SLICE_TRAIL_R:
			gf_fputs("TRAIL_R slice segment", dump);
			break;
		case GF_HEVC_NALU_SLICE_TSA_N:
			gf_fputs("TSA_N slice segment", dump);
			break;
		case GF_HEVC_NALU_SLICE_TSA_R:
			gf_fputs("TSA_R slice segment", dump);
			break;
		case GF_HEVC_NALU_SLICE_STSA_N:
			gf_fputs("STSA_N slice segment", dump);
			break;
		case GF_HEVC_NALU_SLICE_STSA_R:
			gf_fputs("STSA_R slice segment", dump);
			break;
		case GF_HEVC_NALU_SLICE_RADL_N:
			gf_fputs("RADL_N slice segment", dump);
			break;
		case GF_HEVC_NALU_SLICE_RADL_R:
			gf_fputs("RADL_R slice segment", dump);
			break;
		case GF_HEVC_NALU_SLICE_RASL_N:
			gf_fputs("RASL_N slice segment", dump);
			break;
		case GF_HEVC_NALU_SLICE_RASL_R:
			gf_fputs("RASL_R slice segment", dump);
			break;
		case GF_HEVC_NALU_SLICE_BLA_W_LP:
			gf_fputs("Broken link access slice (W LP)", dump);
			break;
		case GF_HEVC_NALU_SLICE_BLA_W_DLP:
			gf_fputs("Broken link access slice (W DLP)", dump);
			break;
		case GF_HEVC_NALU_SLICE_BLA_N_LP:
			gf_fputs("Broken link access slice (N LP)", dump);
			break;
		case GF_HEVC_NALU_SLICE_IDR_W_DLP:
			gf_fputs("IDR slice (W DLP)", dump);
			break;
		case GF_HEVC_NALU_SLICE_IDR_N_LP:
			gf_fputs("IDR slice (N LP)", dump);
			break;
		case GF_HEVC_NALU_SLICE_CRA:
			gf_fputs("CRA slice", dump);
			break;

		case GF_HEVC_NALU_VID_PARAM:
			gf_fputs("Video Parameter Set", dump);
			idx = gf_media_hevc_read_vps(ptr, ptr_size, hevc);
			if (idx<0) gf_fprintf(dump, "\" vps_id=\"PARSING FAILURE");
			else gf_fprintf(dump, "\" vps_id=\"%d", idx);
			break;
		case GF_HEVC_NALU_SEQ_PARAM:
			idx = gf_media_hevc_read_sps(ptr, ptr_size, hevc);
			gf_fputs("Sequence Parameter Set", dump);
			if (idx<0) gf_fprintf(dump, "\" sps_id=\"PARSING FAILURE");
			else {
				HEVC_SPS *sps= &hevc->sps[idx];
				gf_fprintf(dump, "\" sps_id=\"%d", idx);
				if (gf_sys_is_test_mode()) break;

				gf_fprintf(dump, "\" aspect_ratio_info_present_flag=\"%d", sps->aspect_ratio_info_present_flag);
				gf_fprintf(dump, "\" bit_depth_chroma=\"%d", sps->bit_depth_chroma);
				gf_fprintf(dump, "\" bit_depth_luma=\"%d", sps->bit_depth_luma);
				gf_fprintf(dump, "\" chroma_format_idc=\"%d", sps->chroma_format_idc);
				gf_fprintf(dump, "\" colour_description_present_flag=\"%d", sps->colour_description_present_flag);
				gf_fprintf(dump, "\" colour_primaries=\"%d", sps->colour_primaries);
				gf_fprintf(dump, "\" cw_flag=\"%d", sps->cw_flag);
				if (sps->cw_flag) {
					gf_fprintf(dump, "\" cw_bottom=\"%d", sps->cw_bottom);
					gf_fprintf(dump, "\" cw_top=\"%d", sps->cw_top);
					gf_fprintf(dump, "\" cw_left=\"%d", sps->cw_left);
					gf_fprintf(dump, "\" cw_bottom=\"%d", sps->cw_bottom);
				}
				gf_fprintf(dump, "\" height=\"%d", sps->height);
				gf_fprintf(dump, "\" width=\"%d", sps->width);
				gf_fprintf(dump, "\" log2_max_pic_order_cnt_lsb=\"%d", sps->log2_max_pic_order_cnt_lsb);
				gf_fprintf(dump, "\" long_term_ref_pics_present_flag=\"%d", sps->long_term_ref_pics_present_flag);
				gf_fprintf(dump, "\" matrix_coeffs=\"%d", sps->matrix_coeffs);
				gf_fprintf(dump, "\" max_CU_depth=\"%d", sps->max_CU_depth);
				gf_fprintf(dump, "\" max_CU_width=\"%d", sps->max_CU_width);
				gf_fprintf(dump, "\" max_CU_height=\"%d", sps->max_CU_height);
				gf_fprintf(dump, "\" num_long_term_ref_pic_sps=\"%d", sps->num_long_term_ref_pic_sps);
				gf_fprintf(dump, "\" num_short_term_ref_pic_sets=\"%d", sps->num_short_term_ref_pic_sets);
				gf_fprintf(dump, "\" has_timing_info=\"%d", sps->has_timing_info);
				if (sps->has_timing_info) {
					gf_fprintf(dump, "\" time_scale=\"%d", sps->time_scale);
					gf_fprintf(dump, "\" num_ticks_poc_diff_one_minus1=\"%d", sps->num_ticks_poc_diff_one_minus1);
					gf_fprintf(dump, "\" num_units_in_tick=\"%d", sps->num_units_in_tick);
					gf_fprintf(dump, "\" poc_proportional_to_timing_flag=\"%d", sps->poc_proportional_to_timing_flag);
				}
				gf_fprintf(dump, "\" rep_format_idx=\"%d", sps->rep_format_idx);
				gf_fprintf(dump, "\" sample_adaptive_offset_enabled_flag=\"%d", sps->sample_adaptive_offset_enabled_flag);
				gf_fprintf(dump, "\" sar_idc=\"%d", sps->sar_idc);
				gf_fprintf(dump, "\" separate_colour_plane_flag=\"%d", sps->separate_colour_plane_flag);
				gf_fprintf(dump, "\" temporal_mvp_enable_flag=\"%d", sps->temporal_mvp_enable_flag);
				gf_fprintf(dump, "\" transfer_characteristic=\"%d", sps->transfer_characteristic);
				gf_fprintf(dump, "\" video_full_range_flag=\"%d", sps->video_full_range_flag);
				gf_fprintf(dump, "\" sps_ext_or_max_sub_layers_minus1=\"%d", sps->sps_ext_or_max_sub_layers_minus1);
				gf_fprintf(dump, "\" max_sub_layers_minus1=\"%d", sps->max_sub_layers_minus1);
				gf_fprintf(dump, "\" update_rep_format_flag=\"%d", sps->update_rep_format_flag);
				gf_fprintf(dump, "\" sub_layer_ordering_info_present_flag=\"%d", sps->sub_layer_ordering_info_present_flag);
				gf_fprintf(dump, "\" scaling_list_enable_flag=\"%d", sps->scaling_list_enable_flag);
				gf_fprintf(dump, "\" infer_scaling_list_flag=\"%d", sps->infer_scaling_list_flag);
				gf_fprintf(dump, "\" scaling_list_ref_layer_id=\"%d", sps->scaling_list_ref_layer_id);
				gf_fprintf(dump, "\" scaling_list_data_present_flag=\"%d", sps->scaling_list_data_present_flag);
				gf_fprintf(dump, "\" asymmetric_motion_partitions_enabled_flag=\"%d", sps->asymmetric_motion_partitions_enabled_flag);
				gf_fprintf(dump, "\" pcm_enabled_flag=\"%d", sps->pcm_enabled_flag);
				gf_fprintf(dump, "\" strong_intra_smoothing_enable_flag=\"%d", sps->strong_intra_smoothing_enable_flag);
				gf_fprintf(dump, "\" vui_parameters_present_flag=\"%d", sps->vui_parameters_present_flag);
				gf_fprintf(dump, "\" log2_diff_max_min_luma_coding_block_size=\"%d", sps->log2_diff_max_min_luma_coding_block_size);
				gf_fprintf(dump, "\" log2_min_transform_block_size=\"%d", sps->log2_min_transform_block_size);
				gf_fprintf(dump, "\" log2_min_luma_coding_block_size=\"%d", sps->log2_min_luma_coding_block_size);
				gf_fprintf(dump, "\" log2_max_transform_block_size=\"%d", sps->log2_max_transform_block_size);
				gf_fprintf(dump, "\" max_transform_hierarchy_depth_inter=\"%d", sps->max_transform_hierarchy_depth_inter);
				gf_fprintf(dump, "\" max_transform_hierarchy_depth_intra=\"%d", sps->max_transform_hierarchy_depth_intra);
				gf_fprintf(dump, "\" pcm_sample_bit_depth_luma_minus1=\"%d", sps->pcm_sample_bit_depth_luma_minus1);
				gf_fprintf(dump, "\" pcm_sample_bit_depth_chroma_minus1=\"%d", sps->pcm_sample_bit_depth_chroma_minus1);
				gf_fprintf(dump, "\" pcm_loop_filter_disable_flag=\"%d", sps->pcm_loop_filter_disable_flag);
				gf_fprintf(dump, "\" log2_min_pcm_luma_coding_block_size_minus3=\"%d", sps->log2_min_pcm_luma_coding_block_size_minus3);
				gf_fprintf(dump, "\" log2_diff_max_min_pcm_luma_coding_block_size=\"%d", sps->log2_diff_max_min_pcm_luma_coding_block_size);
				gf_fprintf(dump, "\" overscan_info_present=\"%d", sps->overscan_info_present);
				gf_fprintf(dump, "\" overscan_appropriate=\"%d", sps->overscan_appropriate);
				gf_fprintf(dump, "\" video_signal_type_present_flag=\"%d", sps->video_signal_type_present_flag);
				gf_fprintf(dump, "\" video_format=\"%d", sps->video_format);
				gf_fprintf(dump, "\" chroma_loc_info_present_flag=\"%d", sps->chroma_loc_info_present_flag);
				gf_fprintf(dump, "\" chroma_sample_loc_type_top_field=\"%d", sps->chroma_sample_loc_type_top_field);
				gf_fprintf(dump, "\" chroma_sample_loc_type_bottom_field=\"%d", sps->chroma_sample_loc_type_bottom_field);
				gf_fprintf(dump, "\" neutra_chroma_indication_flag=\"%d", sps->neutra_chroma_indication_flag);
				gf_fprintf(dump, "\" field_seq_flag=\"%d", sps->field_seq_flag);
				gf_fprintf(dump, "\" frame_field_info_present_flag=\"%d", sps->frame_field_info_present_flag);
				gf_fprintf(dump, "\" default_display_window_flag=\"%d", sps->default_display_window_flag);
				gf_fprintf(dump, "\" left_offset=\"%d", sps->left_offset);
				gf_fprintf(dump, "\" right_offset=\"%d", sps->right_offset);
				gf_fprintf(dump, "\" top_offset=\"%d", sps->top_offset);
				gf_fprintf(dump, "\" bottom_offset=\"%d", sps->bottom_offset);
				gf_fprintf(dump, "\" hrd_parameters_present_flag=\"%d", sps->hrd_parameters_present_flag);
			}
			break;
		case GF_HEVC_NALU_PIC_PARAM:
			idx = gf_media_hevc_read_pps(ptr, ptr_size, hevc);
			gf_fputs("Picture Parameter Set", dump);
			if (idx<0) gf_fprintf(dump, "\" pps_id=\"PARSING FAILURE");
			else {
				HEVC_PPS *pps= &hevc->pps[idx];
				gf_fprintf(dump, "\" pps_id=\"%d", idx);

				if (gf_sys_is_test_mode()) break;

				gf_fprintf(dump, "\" cabac_init_present_flag=\"%d", pps->cabac_init_present_flag);
				gf_fprintf(dump, "\" dependent_slice_segments_enabled_flag=\"%d", pps->dependent_slice_segments_enabled_flag);
				gf_fprintf(dump, "\" entropy_coding_sync_enabled_flag=\"%d", pps->entropy_coding_sync_enabled_flag);
				gf_fprintf(dump, "\" lists_modification_present_flag=\"%d", pps->lists_modification_present_flag);
				gf_fprintf(dump, "\" loop_filter_across_slices_enabled_flag=\"%d", pps->loop_filter_across_slices_enabled_flag);
				gf_fprintf(dump, "\" loop_filter_across_tiles_enabled_flag=\"%d", pps->loop_filter_across_tiles_enabled_flag);
				gf_fprintf(dump, "\" num_extra_slice_header_bits=\"%d", pps->num_extra_slice_header_bits);
				gf_fprintf(dump, "\" num_ref_idx_l0_default_active=\"%d", pps->num_ref_idx_l0_default_active);
				gf_fprintf(dump, "\" num_ref_idx_l1_default_active=\"%d", pps->num_ref_idx_l1_default_active);
				gf_fprintf(dump, "\" tiles_enabled_flag=\"%d", pps->tiles_enabled_flag);
				if (pps->tiles_enabled_flag) {
					gf_fprintf(dump, "\" uniform_spacing_flag=\"%d", pps->uniform_spacing_flag);
					if (!pps->uniform_spacing_flag) {
						u32 k;
						gf_fprintf(dump, "\" num_tile_columns=\"%d", pps->num_tile_columns);
						gf_fprintf(dump, "\" num_tile_rows=\"%d", pps->num_tile_rows);
						gf_fprintf(dump, "\" colomns_width=\"");
						for (k=0; k<pps->num_tile_columns-1; k++)
							gf_fprintf(dump, "%d ", pps->column_width[k]);
						gf_fprintf(dump, "\" rows_height=\"");
						for (k=0; k<pps->num_tile_rows-1; k++)
							gf_fprintf(dump, "%d ", pps->row_height[k]);
					}
				}
				gf_fprintf(dump, "\" output_flag_present_flag=\"%d", pps->output_flag_present_flag);
				gf_fprintf(dump, "\" pic_init_qp_minus26=\"%d", pps->pic_init_qp_minus26);
				gf_fprintf(dump, "\" slice_chroma_qp_offsets_present_flag=\"%d", pps->slice_chroma_qp_offsets_present_flag);
				gf_fprintf(dump, "\" slice_segment_header_extension_present_flag=\"%d", pps->slice_segment_header_extension_present_flag);
				gf_fprintf(dump, "\" weighted_pred_flag=\"%d", pps->weighted_pred_flag);
				gf_fprintf(dump, "\" weighted_bipred_flag=\"%d", pps->weighted_bipred_flag);

				gf_fprintf(dump, "\" sign_data_hiding_flag=\"%d", pps->sign_data_hiding_flag);
				gf_fprintf(dump, "\" constrained_intra_pred_flag=\"%d", pps->constrained_intra_pred_flag);
				gf_fprintf(dump, "\" transform_skip_enabled_flag=\"%d", pps->transform_skip_enabled_flag);
				gf_fprintf(dump, "\" cu_qp_delta_enabled_flag=\"%d", pps->cu_qp_delta_enabled_flag);
				if (pps->cu_qp_delta_enabled_flag)
					gf_fprintf(dump, "\" diff_cu_qp_delta_depth=\"%d", pps->diff_cu_qp_delta_depth);
				gf_fprintf(dump, "\" transquant_bypass_enable_flag=\"%d", pps->transquant_bypass_enable_flag);
				gf_fprintf(dump, "\" pic_cb_qp_offset=\"%d", pps->pic_cb_qp_offset);
				gf_fprintf(dump, "\" pic_cr_qp_offset=\"%d", pps->pic_cr_qp_offset);

				gf_fprintf(dump, "\" deblocking_filter_control_present_flag=\"%d", pps->deblocking_filter_control_present_flag);
				if (pps->deblocking_filter_control_present_flag) {
					gf_fprintf(dump, "\" deblocking_filter_override_enabled_flag=\"%d", pps->deblocking_filter_override_enabled_flag);
					gf_fprintf(dump, "\" pic_disable_deblocking_filter_flag=\"%d", pps->pic_disable_deblocking_filter_flag);
					gf_fprintf(dump, "\" beta_offset_div2=\"%d", pps->beta_offset_div2);
					gf_fprintf(dump, "\" tc_offset_div2=\"%d", pps->tc_offset_div2);
				}
				gf_fprintf(dump, "\" pic_scaling_list_data_present_flag=\"%d", pps->pic_scaling_list_data_present_flag);
				gf_fprintf(dump, "\" log2_parallel_merge_level_minus2=\"%d", pps->log2_parallel_merge_level_minus2);
			}
			break;
		case GF_HEVC_NALU_ACCESS_UNIT:
			gf_fputs("AU Delimiter", dump);
			gf_fprintf(dump, "\" primary_pic_type=\"%d", ptr[2] >> 5);
			break;
		case GF_HEVC_NALU_END_OF_SEQ:
			gf_fputs("End of Sequence", dump);
			break;
		case GF_HEVC_NALU_END_OF_STREAM:
			gf_fputs("End of Stream", dump);
			break;
		case GF_HEVC_NALU_FILLER_DATA:
			gf_fputs("Filler Data", dump);
			break;
		case GF_HEVC_NALU_SEI_PREFIX:
			gf_fputs("SEI Prefix", dump);
			break;
		case GF_HEVC_NALU_SEI_SUFFIX:
			gf_fputs("SEI Suffix", dump);
			break;
		case 48:
			gf_fputs("HEVCAggregator", dump);
			break;
		case 49:
		{
			u32 remain = ptr_size-2;
			char *s = ptr+2;

			gf_fputs("HEVCExtractor ", dump);

			while (remain) {
				u32 mode = s[0];
				remain -= 1;
				s += 1;
				if (mode) {
					u32 len = s[0];
					if (len+1>remain) {
						gf_fprintf(dump, "error=\"invalid inband data extractor size: %d vs %d remaining\"", len, remain);
						return;
					}
					remain -= len+1;
					s += len+1;
					gf_fprintf(dump, "\" inband_size=\"%d", len);
				} else {
					if (remain < 2 + 2*nalh_size) {
						gf_fprintf(dump, "error=\"invalid ref data extractor size: %d vs %d remaining\"", 2 + 2*nalh_size, remain);
						return;
					}
					track_ref_index = (u8) s[0];
					sample_offset = (s8) s[1];
					data_offset = inspect_get_nal_size(&s[2], nalh_size);
					data_size = inspect_get_nal_size(&s[2+nalh_size], nalh_size);
					gf_fprintf(dump, "\" track_ref_index=\"%d\" sample_offset=\"%d\" data_offset=\"%d\" data_size=\"%d", track_ref_index, sample_offset, data_offset, data_size);

					remain -= 2 + 2*nalh_size;
					s += 2 + 2*nalh_size;
				}
			}
		}
			break;
		default:
			gf_fprintf(dump, "UNKNOWN (parsing return %d)", res);
			break;
		}
		gf_fputs("\"", dump);

		if ((type == GF_HEVC_NALU_SEI_PREFIX) || (type == GF_HEVC_NALU_SEI_SUFFIX)) {
			bs = gf_bs_new(ptr, ptr_size, GF_BITSTREAM_READ);
			dump_sei(dump, bs, GF_TRUE);
			gf_bs_del(bs);
		}

		if (type < GF_HEVC_NALU_VID_PARAM) {
			gf_fprintf(dump, " slice=\"%s\" poc=\"%d\"", (hevc->s_info.slice_type==GF_HEVC_SLICE_TYPE_I) ? "I" : (hevc->s_info.slice_type==GF_HEVC_SLICE_TYPE_P) ? "P" : (hevc->s_info.slice_type==GF_HEVC_SLICE_TYPE_B) ? "B" : "Unknown", hevc->s_info.poc);
			gf_fprintf(dump, " first_slice_in_pic=\"%d\"", hevc->s_info.first_slice_segment_in_pic_flag);
			gf_fprintf(dump, " dependent_slice_segment=\"%d\"", hevc->s_info.dependent_slice_segment_flag);

			if (!gf_sys_is_test_mode()) {
				gf_fprintf(dump, " redundant_pic_cnt=\"%d\"", hevc->s_info.redundant_pic_cnt);
				gf_fprintf(dump, " slice_qp_delta=\"%d\"", hevc->s_info.slice_qp_delta);
				gf_fprintf(dump, " slice_segment_address=\"%d\"", hevc->s_info.slice_segment_address);
				gf_fprintf(dump, " slice_type=\"%d\"", hevc->s_info.slice_type);
			}
		}

		gf_fprintf(dump, " layer_id=\"%d\" temporal_id=\"%d\"", quality_id, temporal_id);

#endif //GPAC_DISABLE_HEVC
		return;
	}

	type = ptr[0] & 0x1F;
	nal_ref_idc = ptr[0] & 0x60;
	nal_ref_idc>>=5;
	gf_fprintf(dump, "code=\"%d\" type=\"", type);
	res = -2;
	bs = gf_bs_new(ptr, ptr_size, GF_BITSTREAM_READ);
	switch (type) {
	case GF_AVC_NALU_NON_IDR_SLICE:
		gf_fputs("Non IDR slice", dump);
		if (is_encrypted) break;
		res = gf_media_avc_parse_nalu(bs, avc);
		break;
	case GF_AVC_NALU_DP_A_SLICE:
		gf_fputs("DP Type A slice", dump);
		break;
	case GF_AVC_NALU_DP_B_SLICE:
		gf_fputs("DP Type B slice", dump);
		break;
	case GF_AVC_NALU_DP_C_SLICE:
		gf_fputs("DP Type C slice", dump);
		break;
	case GF_AVC_NALU_IDR_SLICE:
		gf_fputs("IDR slice", dump);
		if (is_encrypted) break;
		res = gf_media_avc_parse_nalu(bs, avc);
		break;
	case GF_AVC_NALU_SEI:
		gf_fputs("SEI Message", dump);
		break;
	case GF_AVC_NALU_SEQ_PARAM:
		gf_fputs("SequenceParameterSet", dump);
		if (is_encrypted) break;
		idx = gf_media_avc_read_sps_bs(bs, avc, 0, NULL);
		if (idx<0) gf_fprintf(dump, "\" sps_id=\"PARSING FAILURE");
		else gf_fprintf(dump, "\" sps_id=\"%d", idx);
		gf_fprintf(dump, "\" frame_mbs_only_flag=\"%d", avc->sps->frame_mbs_only_flag);
		gf_fprintf(dump, "\" mb_adaptive_frame_field_flag=\"%d", avc->sps->mb_adaptive_frame_field_flag);
		gf_fprintf(dump, "\" vui_parameters_present_flag=\"%d", avc->sps->vui_parameters_present_flag);
		gf_fprintf(dump, "\" max_num_ref_frames=\"%d", avc->sps->max_num_ref_frames);
		gf_fprintf(dump, "\" gaps_in_frame_num_value_allowed_flag=\"%d", avc->sps->gaps_in_frame_num_value_allowed_flag);
		gf_fprintf(dump, "\" chroma_format_idc=\"%d", avc->sps->chroma_format);
		gf_fprintf(dump, "\" bit_depth_luma_minus8=\"%d", avc->sps->luma_bit_depth_m8);
		gf_fprintf(dump, "\" bit_depth_chroma_minus8=\"%d", avc->sps->chroma_bit_depth_m8);
		gf_fprintf(dump, "\" width=\"%d", avc->sps->width);
		gf_fprintf(dump, "\" height=\"%d", avc->sps->height);
		gf_fprintf(dump, "\" crop_top=\"%d", avc->sps->crop.top);
		gf_fprintf(dump, "\" crop_left=\"%d", avc->sps->crop.left);
		gf_fprintf(dump, "\" crop_bottom=\"%d", avc->sps->crop.bottom);
		gf_fprintf(dump, "\" crop_right=\"%d", avc->sps->crop.right);
		if (avc->sps->vui_parameters_present_flag) {
			gf_fprintf(dump, "\" vui_video_full_range_flag=\"%d", avc->sps->vui.video_full_range_flag);
			gf_fprintf(dump, "\" vui_video_signal_type_present_flag=\"%d", avc->sps->vui.video_signal_type_present_flag);
			gf_fprintf(dump, "\" vui_aspect_ratio_info_present_flag=\"%d", avc->sps->vui.aspect_ratio_info_present_flag);
			gf_fprintf(dump, "\" vui_aspect_ratio_num=\"%d", avc->sps->vui.par_num);
			gf_fprintf(dump, "\" vui_aspect_ratio_den=\"%d", avc->sps->vui.par_den);
			gf_fprintf(dump, "\" vui_overscan_info_present_flag=\"%d", avc->sps->vui.overscan_info_present_flag);
			gf_fprintf(dump, "\" vui_colour_description_present_flag=\"%d", avc->sps->vui.colour_description_present_flag);
			gf_fprintf(dump, "\" vui_colour_primaries=\"%d", avc->sps->vui.colour_primaries);
			gf_fprintf(dump, "\" vui_transfer_characteristics=\"%d", avc->sps->vui.transfer_characteristics);
			gf_fprintf(dump, "\" vui_matrix_coefficients=\"%d", avc->sps->vui.matrix_coefficients);
			gf_fprintf(dump, "\" vui_low_delay_hrd_flag=\"%d", avc->sps->vui.low_delay_hrd_flag);
		}
		if (gf_sys_is_test_mode()) break;
		gf_fprintf(dump, "\" log2_max_poc_lsb=\"%d", avc->sps->log2_max_poc_lsb);
		gf_fprintf(dump, "\" log2_max_frame_num=\"%d", avc->sps->log2_max_frame_num);
		gf_fprintf(dump, "\" delta_pic_order_always_zero_flag=\"%d", avc->sps->delta_pic_order_always_zero_flag);
		gf_fprintf(dump, "\" offset_for_non_ref_pic=\"%d", avc->sps->offset_for_non_ref_pic);

		break;
	case GF_AVC_NALU_PIC_PARAM:
		gf_fputs("PictureParameterSet", dump);
		if (is_encrypted) break;
		idx = gf_media_avc_read_pps_bs(bs, avc);
		if (idx<0) gf_fprintf(dump, "\" pps_id=\"PARSING FAILURE\" ");
		else gf_fprintf(dump, "\" pps_id=\"%d\" sps_id=\"%d", idx, avc->pps[idx].sps_id);
		gf_fprintf(dump, "\" entropy_coding_mode_flag=\"%d", avc->pps[idx].entropy_coding_mode_flag);
		if (gf_sys_is_test_mode()) break;
		gf_fprintf(dump, "\" deblocking_filter_control_present_flag=\"%d", avc->pps[idx].deblocking_filter_control_present_flag);
		gf_fprintf(dump, "\" mb_slice_group_map_type=\"%d", avc->pps[idx].mb_slice_group_map_type);
		gf_fprintf(dump, "\" num_ref_idx_l0_default_active_minus1=\"%d", avc->pps[idx].num_ref_idx_l0_default_active_minus1);
		gf_fprintf(dump, "\" num_ref_idx_l1_default_active_minus1=\"%d", avc->pps[idx].num_ref_idx_l1_default_active_minus1);
		gf_fprintf(dump, "\" pic_order_present=\"%d", avc->pps[idx].pic_order_present);
		gf_fprintf(dump, "\" pic_size_in_map_units_minus1=\"%d", avc->pps[idx].pic_size_in_map_units_minus1);
		gf_fprintf(dump, "\" redundant_pic_cnt_present=\"%d", avc->pps[idx].redundant_pic_cnt_present);
		gf_fprintf(dump, "\" slice_group_change_rate_minus1=\"%d", avc->pps[idx].slice_group_change_rate_minus1);
		gf_fprintf(dump, "\" slice_group_count=\"%d", avc->pps[idx].slice_group_count);
		gf_fprintf(dump, "\" weighted_pred_flag=\"%d", avc->pps[idx].weighted_pred_flag);
		gf_fprintf(dump, "\" weighted_bipred_idc=\"%d", avc->pps[idx].weighted_bipred_idc);
		break;
	case GF_AVC_NALU_ACCESS_UNIT:
		gf_fputs("AccessUnit delimiter", dump);
		if (is_encrypted) break;
		gf_fprintf(dump, "\" primary_pic_type=\"%d", gf_bs_read_u8(bs) >> 5);
		break;
	case GF_AVC_NALU_END_OF_SEQ:
		gf_fputs("EndOfSequence", dump);
		break;
	case GF_AVC_NALU_END_OF_STREAM:
		gf_fputs("EndOfStream", dump);
		break;
	case GF_AVC_NALU_FILLER_DATA:
		gf_fputs("Filler data", dump);
		break;
	case GF_AVC_NALU_SEQ_PARAM_EXT:
		gf_fputs("SequenceParameterSetExtension", dump);
		break;
	case GF_AVC_NALU_SVC_PREFIX_NALU:
		gf_fputs("SVCPrefix", dump);
		break;
	case GF_AVC_NALU_SVC_SUBSEQ_PARAM:
		gf_fputs("SVCSubsequenceParameterSet", dump);
		if (is_encrypted) break;
		idx = gf_media_avc_read_sps_bs(bs, avc, 1, NULL);
		assert (idx >= 0);
		gf_fprintf(dump, "\" sps_id=\"%d", idx - GF_SVC_SSPS_ID_SHIFT);
		break;
	case GF_AVC_NALU_SLICE_AUX:
		gf_fputs("Auxiliary Slice", dump);
		break;

	case GF_AVC_NALU_SVC_SLICE:
		gf_fputs(is_svc ? "SVCSlice" : "CodedSliceExtension", dump);
		if (is_encrypted) break;
		gf_media_avc_parse_nalu(bs, avc);
		dependency_id = (ptr[2] & 0x70) >> 4;
		quality_id = (ptr[2] & 0x0F);
		temporal_id = (ptr[3] & 0xE0) >> 5;
		gf_fprintf(dump, "\" dependency_id=\"%d\" quality_id=\"%d\" temporal_id=\"%d", dependency_id, quality_id, temporal_id);
		gf_fprintf(dump, "\" poc=\"%d", avc->s_info.poc);
		break;
	case 30:
		gf_fputs("SVCAggregator", dump);
		break;
	case 31:
		gf_fputs("SVCExtractor", dump);
		if (is_encrypted) break;
		track_ref_index = (u8) ptr[4];
		sample_offset = (s8) ptr[5];
		data_offset = inspect_get_nal_size(&ptr[6], nalh_size);
		data_size = inspect_get_nal_size(&ptr[6+nalh_size], nalh_size);
		gf_fprintf(dump, "\" track_ref_index=\"%d\" sample_offset=\"%d\" data_offset=\"%d\" data_size=\"%d\"", track_ref_index, sample_offset, data_offset, data_size);
		break;

	default:
		gf_fputs("UNKNOWN", dump);
		break;
	}
	gf_fputs("\"", dump);

	if (nal_ref_idc) {
		gf_fprintf(dump, " nal_ref_idc=\"%d\"", nal_ref_idc);
	}
	if (res>=0) {
		gf_fprintf(dump, " poc=\"%d\" pps_id=\"%d\" field_pic_flag=\"%d\"", avc->s_info.poc, avc->s_info.pps->id, (int)avc->s_info.field_pic_flag);
	}

	if (!is_encrypted) {
		if (type == GF_AVC_NALU_SEI) {
			dump_sei(dump, bs, GF_FALSE);
		}
	}

	if (res == -1)
		gf_fprintf(dump, " status=\"error decoding slice\"");

	if (bs) gf_bs_del(bs);
}

GF_EXPORT
void gf_inspect_dump_obu(FILE *dump, AV1State *av1, u8 *obu, u64 obu_length, ObuType obu_type, u64 obu_size, u32 hdr_size, Bool dump_crc)
{
#define DUMP_OBU_INT(_v) gf_fprintf(dump, #_v"=\"%d\" ", av1->_v);
#define DUMP_OBU_INT2(_n, _v) gf_fprintf(dump, _n"=\"%d\" ", _v);

	gf_fprintf(dump, "   <OBU size=\""LLU"\" type=\"%s\" header_size=\"%d\" has_size_field=\"%d\" has_ext=\"%d\" temporalID=\"%d\" spatialID=\"%d\" ", obu_size, av1_get_obu_name(obu_type), hdr_size, av1->obu_has_size_field, av1->obu_extension_flag, av1->temporal_id , av1->spatial_id);
	if (dump_crc && (obu_length<0xFFFFFFFFUL))
		gf_fprintf(dump, "crc=\"%u\" ", gf_crc_32(obu, (u32) obu_length) );
	switch (obu_type) {
	case OBU_SEQUENCE_HEADER:
		DUMP_OBU_INT(width)
		DUMP_OBU_INT(height)
		DUMP_OBU_INT(bit_depth)
		DUMP_OBU_INT(still_picture)
		DUMP_OBU_INT(OperatingPointIdc)
		DUMP_OBU_INT(color_range)
		DUMP_OBU_INT(color_description_present_flag)
		DUMP_OBU_INT(color_primaries)
		DUMP_OBU_INT(transfer_characteristics)
		DUMP_OBU_INT(matrix_coefficients)
		DUMP_OBU_INT2("profile", av1->config->seq_profile)
		DUMP_OBU_INT2("level", av1->config->seq_level_idx_0)
		break;
	case OBU_FRAME_HEADER:
	case OBU_FRAME:
		if (av1->frame_id_numbers_present_flag) {
			DUMP_OBU_INT2("delta_frame_id_length_minus_2", av1->delta_frame_id_length_minus_2)
		}
		if (av1->reduced_still_picture_header) {
			DUMP_OBU_INT(reduced_still_picture_header)
		}
		DUMP_OBU_INT2("uncompressed_header_bytes", av1->frame_state.uncompressed_header_bytes);
		if (av1->frame_state.uncompressed_header_bytes) {
			if (av1->frame_state.frame_type==AV1_KEY_FRAME) gf_fprintf(dump, "frame_type=\"key\" ");
			else if (av1->frame_state.frame_type==AV1_INTER_FRAME) gf_fprintf(dump, "frame_type=\"inter\" ");
			else if (av1->frame_state.frame_type==AV1_INTRA_ONLY_FRAME) gf_fprintf(dump, "frame_type=\"intra_only\" ");
			else if (av1->frame_state.frame_type==AV1_SWITCH_FRAME) gf_fprintf(dump, "frame_type=\"switch\" ");
			gf_fprintf(dump, "refresh_frame_flags=\"%d\" ", av1->frame_state.refresh_frame_flags);

			DUMP_OBU_INT2("show_frame", av1->frame_state.show_frame);
			DUMP_OBU_INT2("show_existing_frame", av1->frame_state.show_existing_frame);
		}
		if (obu_type==OBU_FRAME_HEADER)
			break;

	case OBU_TILE_GROUP:
		if (av1->frame_state.nb_tiles_in_obu) {
			DUMP_OBU_INT2("nb_tiles", av1->frame_state.nb_tiles_in_obu)
		} else {
			gf_fprintf(dump, "nb_tiles=\"unknown\" ");
		}
		break;
	default:
		break;

	}
	gf_fprintf(dump, "/>\n");
}

GF_EXPORT
void gf_inspect_dump_prores(FILE *dump, u8 *ptr, u64 frame_size, Bool dump_crc)
{
	GF_BitStream *bs;
	GF_ProResFrameInfo prores_frame;
	GF_Err e;

	bs = gf_bs_new(ptr, frame_size, GF_BITSTREAM_READ);
	e = gf_media_prores_parse_bs(bs, &prores_frame);
	gf_bs_del(bs);
	if (e) {
		gf_fprintf(dump, "   <!-- Error reading frame %s -->\n", gf_error_to_string(e) );
		return;
	}
	gf_fprintf(dump, "   <ProResFrame framesize=\"%d\" frameID=\"%s\" version=\"%d\""
		, prores_frame.frame_size
		, gf_4cc_to_str(prores_frame.frame_identifier)
		, prores_frame.version
	);
	gf_fprintf(dump, " encoderID=\"%s\" width=\"%d\" height=\"%d\""
		, gf_4cc_to_str(prores_frame.encoder_id)
		, prores_frame.width
		, prores_frame.height
	);
	switch (prores_frame.chroma_format) {
	case 0: gf_fprintf(dump, " chromaFormat=\"reserved(0)\""); break;
	case 1: gf_fprintf(dump, " chromaFormat=\"reserved(1)\""); break;
	case 2: gf_fprintf(dump, " chromaFormat=\"4:2:2\""); break;
	case 3: gf_fprintf(dump, " chromaFormat=\"4:4:4\""); break;
	}
	switch (prores_frame.interlaced_mode) {
	case 0: gf_fprintf(dump, " interlacedMode=\"progressive\""); break;
	case 1: gf_fprintf(dump, " interlacedMode=\"interlaced_top_first\""); break;
	case 2: gf_fprintf(dump, " interlacedMode=\"interlaced_bottom_first\""); break;
	case 3: gf_fprintf(dump, " interlacedMode=\"reserved\""); break;
	}
	switch (prores_frame.aspect_ratio_information) {
	case 0: gf_fprintf(dump, " aspectRatio=\"unknown\""); break;
	case 1: gf_fprintf(dump, " aspectRatio=\"1:1\""); break;
	case 2: gf_fprintf(dump, " aspectRatio=\"4:3\""); break;
	default: gf_fprintf(dump, " aspectRatio=\"reserved(%d)\"", prores_frame.aspect_ratio_information); break;
	}
	switch (prores_frame.framerate_code) {
	case 0: gf_fprintf(dump, " framerate=\"unknown\""); break;
	case 1: gf_fprintf(dump, " framerate=\"23.976\""); break;
	case 2: gf_fprintf(dump, " framerate=\"24\""); break;
	case 3: gf_fprintf(dump, " framerate=\"25\""); break;
	case 4: gf_fprintf(dump, " framerate=\"29.97\""); break;
	case 5: gf_fprintf(dump, " framerate=\"30\""); break;
	case 6: gf_fprintf(dump, " framerate=\"50\""); break;
	case 7: gf_fprintf(dump, " framerate=\"59.94\""); break;
	case 8: gf_fprintf(dump, " framerate=\"60\""); break;
	case 9: gf_fprintf(dump, " framerate=\"100\""); break;
	case 10: gf_fprintf(dump, " framerate=\"119.88\""); break;
	case 11: gf_fprintf(dump, " framerate=\"120\""); break;
	default: gf_fprintf(dump, " framerate=\"reserved(%d)\"", prores_frame.framerate_code); break;
	}
	switch (prores_frame.color_primaries) {
	case 0: case 2: gf_fprintf(dump, " colorPrimaries=\"unknown\""); break;
	case 1: gf_fprintf(dump, " colorPrimaries=\"BT.709\""); break;
	case 5: gf_fprintf(dump, " colorPrimaries=\"BT.601-625\""); break;
	case 6: gf_fprintf(dump, " colorPrimaries=\"BT.601-525\""); break;
	case 9: gf_fprintf(dump, " colorPrimaries=\"BT.2020\""); break;
	case 11: gf_fprintf(dump, " colorPrimaries=\"P3\""); break;
	case 12: gf_fprintf(dump, " colorPrimaries=\"P3-D65\""); break;
	default: gf_fprintf(dump, " colorPrimaries=\"reserved(%d)\"", prores_frame.color_primaries); break;
	}
	switch (prores_frame.matrix_coefficients) {
	case 0: case 2: gf_fprintf(dump, " matrixCoefficients=\"unknown\""); break;
	case 1: gf_fprintf(dump, " matrixCoefficients=\"BT.709\""); break;
	case 6: gf_fprintf(dump, " matrixCoefficients=\"BT.601\""); break;
	case 9: gf_fprintf(dump, " matrixCoefficients=\"BT.2020\""); break;
	default: gf_fprintf(dump, " matrixCoefficients=\"reserved(%d)\"", prores_frame.matrix_coefficients); break;
	}
	switch (prores_frame.alpha_channel_type) {
	case 0: gf_fprintf(dump, " alphaChannel=\"none\""); break;
	case 1: gf_fprintf(dump, " alphaChannel=\"8bits\""); break;
	case 2: gf_fprintf(dump, " alphaChannel=\"16bits\""); break;
	default: gf_fprintf(dump, " alphaChannel=\"reserved(%d)\"", prores_frame.alpha_channel_type); break;
	}
	gf_fprintf(dump, " transferCharacteristics=\"%d\" numPictures=\"%d\"" , prores_frame.transfer_characteristics, prores_frame.nb_pic);

	if (!prores_frame.load_luma_quant_matrix && !prores_frame.load_chroma_quant_matrix) {
		gf_fprintf(dump, "/>\n");
	} else {
		u32 j, k;
		gf_fprintf(dump, ">\n");
		if (prores_frame.load_luma_quant_matrix) {
			gf_fprintf(dump, "    <LumaQuantMatrix coefs=\"");
			for (j=0; j<8; j++) {
				for (k=0; k<8; k++) {
					gf_fprintf(dump, " %02X", prores_frame.luma_quant_matrix[j][k]);
				}
			}
			gf_fprintf(dump, "\">\n");
		}
		if (prores_frame.load_chroma_quant_matrix) {
			gf_fprintf(dump, "    <ChromaQuantMatrix coefs=\"");
			for (j=0; j<8; j++) {
				for (k=0; k<8; k++) {
					gf_fprintf(dump, " %02X", prores_frame.chroma_quant_matrix[j][k]);
				}
			}
			gf_fprintf(dump, "\">\n");
		}
		gf_fprintf(dump, "   </ProResFrame>\n");
	}
}
#endif



static void inspect_finalize(GF_Filter *filter)
{
	char szLine[1025];

	Bool concat=GF_FALSE;
	GF_InspectCtx *ctx = (GF_InspectCtx *) gf_filter_get_udta(filter);

	if (ctx->dump) {
		if ((ctx->dump!=stderr) && (ctx->dump!=stdout)) concat=GF_TRUE;
		else if (!ctx->interleave) concat=GF_TRUE;
	}
	while (gf_list_count(ctx->src_pids)) {
		PidCtx *pctx = gf_list_pop_front(ctx->src_pids);

		if (!ctx->interleave && pctx->tmp) {
			if (concat) {
				gf_fseek(pctx->tmp, 0, SEEK_SET);
				while (!gf_feof(pctx->tmp)) {
					u32 read = (u32) gf_fread(szLine, 1024, pctx->tmp);
					gf_fwrite(szLine, read, ctx->dump);
				}
			}
			gf_fclose(pctx->tmp);
			if (ctx->xml)
				gf_fprintf(ctx->dump, "</PIDInspect>");
		}
#ifndef GPAC_DISABLE_AV_PARSERS
		if (pctx->avc_state) gf_free(pctx->avc_state);
		if (pctx->hevc_state) gf_free(pctx->hevc_state);
		if (pctx->av1_state) {
			if (pctx->av1_state->config) gf_odf_av1_cfg_del(pctx->av1_state->config);
			gf_free(pctx->av1_state);
		}
#endif
		gf_free(pctx);
	}
	gf_list_del(ctx->src_pids);

	if (ctx->dump) {
		if (ctx->xml) gf_fprintf(ctx->dump, "</GPACInspect>\n");
		if ((ctx->dump!=stderr) && (ctx->dump!=stdout))
			gf_fclose(ctx->dump);
	}

}

static void inspect_dump_property(GF_InspectCtx *ctx, FILE *dump, u32 p4cc, const char *pname, const GF_PropertyValue *att)
{
	char szDump[GF_PROP_DUMP_ARG_SIZE];

	if (!pname) pname = gf_props_4cc_get_name(p4cc);

	if (p4cc==GF_PROP_PID_DOWNLOAD_SESSION)
		return;

	if (gf_sys_is_test_mode() || ctx->test) {
		switch (p4cc) {
		case GF_PROP_PID_FILEPATH:
		case GF_PROP_PID_URL:
			return;
		case GF_PROP_PID_FILE_CACHED:
		case GF_PROP_PID_DURATION:
			if (ctx->test==INSPECT_TEST_NETWORK)
				return;
			break;
		case GF_PROP_PID_DECODER_CONFIG:
		case GF_PROP_PID_DECODER_CONFIG_ENHANCEMENT:
		case GF_PROP_PID_DOWN_SIZE:
			if (ctx->test==INSPECT_TEST_ENCODE)
				return;
			break;
		case GF_PROP_PID_ISOM_TREX_TEMPLATE:
		case GF_PROP_PID_ISOM_STSD_TEMPLATE:
			//TODO once all OK: remove this test and regenerate all hashes
			if (gf_sys_is_test_mode())
				return;
		default:
			if (gf_sys_is_test_mode() && (att->type==GF_PROP_POINTER) )
				return;
			break;
		}
	}

	if (ctx->xml) {
		if (ctx->dtype)
			gf_fprintf(dump, " type=\"%s\"", gf_props_get_type_name(att->type) );
			
		if (pname && strchr(pname, ' ')) {
			u32 i=0;
			char *pname_no_space = gf_strdup(pname);
			while (pname_no_space[i]) {
				if (pname_no_space[i]==' ') pname_no_space[i]='_';
				i++;
			}

			if (att->type==GF_PROP_UINT_LIST) {
				for (u32 k = 0; k < att->value.uint_list.nb_items; k++) {
					if (k) gf_fprintf(dump, ", ");
					switch (p4cc) {
					case GF_PROP_PID_ISOM_BRANDS:
						if (! gf_sys_is_test_mode()) {
							gf_fprintf(dump, "%s", gf_4cc_to_str(att->value.uint_list.vals[k]) );
							break;
						}
					default:
						gf_fprintf(dump, "%d", att->value.uint_list.vals[k]);
						break;
					}
				}
			} else if (att->type==GF_PROP_STRING_LIST) {
				u32 plist_count = gf_list_count(att->value.string_list);
				for (u32 k = 0; k < plist_count; k++) {
					if (k) gf_fprintf(dump, ", ");
					gf_xml_dump_string(dump, NULL, (char *) gf_list_get(att->value.string_list, k), NULL);
				}
			} else if ((att->type==GF_PROP_STRING) || (att->type==GF_PROP_STRING_NO_COPY)) {
				gf_xml_dump_string(dump, NULL, att->value.string, NULL);
			} else {
				gf_fprintf(dump, " %s=\"%s\"", pname_no_space, gf_props_dump(p4cc, att, szDump, ctx->dump_data));
			}
			gf_free(pname_no_space);
		} else {
			gf_fprintf(dump, " %s=\"%s\"", pname ? pname : gf_4cc_to_str(p4cc), gf_props_dump(p4cc, att, szDump, ctx->dump_data));
		}
	} else {
		if (ctx->dtype) {
			gf_fprintf(dump, "\t%s (%s): ", pname ? pname : gf_4cc_to_str(p4cc), gf_props_get_type_name(att->type));
		} else {
			gf_fprintf(dump, "\t%s: ", pname ? pname : gf_4cc_to_str(p4cc));
		}

		if (att->type==GF_PROP_UINT_LIST) {
			for (u32 k = 0; k < att->value.uint_list.nb_items; k++) {
				if (k) gf_fprintf(dump, ", ");
				switch (p4cc) {
				case GF_PROP_PID_ISOM_BRANDS:
					if (! gf_sys_is_test_mode()) {
						gf_fprintf(dump, "%s", gf_4cc_to_str(att->value.uint_list.vals[k]) );
						break;
					}
				default:
					gf_fprintf(dump, "%d", att->value.uint_list.vals[k]);
					break;
				}
			}
		} else if (att->type==GF_PROP_STRING_LIST) {
			u32 plist_count = gf_list_count(att->value.string_list);
			for (u32 k = 0; k < plist_count; k++) {
				if (k) gf_fprintf(dump, ", ");
				gf_fprintf(dump, "%s", (const char *) gf_list_get(att->value.string_list, k));
			}
		}else{
			gf_fprintf(dump, "%s", gf_props_dump(p4cc, att, szDump, ctx->dump_data) );
		}
		gf_fprintf(dump, "\n");
	}
}

static void inspect_dump_packet_fmt(GF_InspectCtx *ctx, FILE *dump, GF_FilterPacket *pck, PidCtx *pctx, u64 pck_num)
{
	char szDump[GF_PROP_DUMP_ARG_SIZE];
	u32 size=0;
	const char *data=NULL;
	char *str = ctx->fmt;
	assert(str);

	if (pck)
		data = gf_filter_pck_get_data(pck, &size);

	while (str) {
		char csep;
		char *sep, *key;
		if (!str[0]) break;

		if ((str[0] != '$') && (str[0] != '%') && (str[0] != '@')) {
			gf_fprintf(dump, "%c", str[0]);
			str = str+1;
			continue;
		}
		csep = str[0];
		if (str[1] == csep) {
			gf_fprintf(dump, "%c", str[0]);
			str = str+2;
			continue;
		}
		sep = strchr(str+1, csep);
		if (!sep) {
			gf_fprintf(dump, "%c", str[0]);
			str = str+1;
			continue;
		}
		sep[0] = 0;
		key = str+1;

		if (!pck) {
			if (!strcmp(key, "lf")) gf_fprintf(dump, "\n" );
			else if (!strcmp(key, "cr")) gf_fprintf(dump, "\r" );
			else if (!strncmp(key, "pid.", 4)) gf_fprintf(dump, "%s", key+4);
			else gf_fprintf(dump, "%s", key);
			sep[0] = csep;
			str = sep+1;
			continue;
		}

		if (!strcmp(key, "pn")) gf_fprintf(dump, LLU, pck_num);
		else if (!strcmp(key, "dts")) {
			u64 ts = gf_filter_pck_get_dts(pck);
			if (ts==GF_FILTER_NO_TS) gf_fprintf(dump, "N/A");
			else gf_fprintf(dump, LLU, ts );
		}
		else if (!strcmp(key, "cts")) {
			u64 ts = gf_filter_pck_get_cts(pck);
			if (ts==GF_FILTER_NO_TS) gf_fprintf(dump, "N/A");
			else gf_fprintf(dump, LLU, ts );
		}
		else if (!strcmp(key, "ddts")) {
			u64 ts = gf_filter_pck_get_dts(pck);
			if ((ts==GF_FILTER_NO_TS) || (pctx->prev_dts==GF_FILTER_NO_TS)) gf_fprintf(dump, "N/A");
			else {
				s64 diff = ts;
				diff -= pctx->prev_dts;
				gf_fprintf(dump, LLD, diff);
			}
			pctx->prev_dts = ts;
		}
		else if (!strcmp(key, "dcts")) {
			u64 ts = gf_filter_pck_get_cts(pck);
			if ((ts==GF_FILTER_NO_TS) || (pctx->prev_cts==GF_FILTER_NO_TS)) gf_fprintf(dump, "N/A");
			else {
				s64 diff = ts;
				diff -= pctx->prev_cts;
				gf_fprintf(dump, LLD, diff);
			}
			pctx->prev_cts = ts;
		}
		else if (!strcmp(key, "ctso")) {
			u64 dts = gf_filter_pck_get_dts(pck);
			u64 cts = gf_filter_pck_get_cts(pck);
			if (dts==GF_FILTER_NO_TS) dts = cts;
			if (cts==GF_FILTER_NO_TS) gf_fprintf(dump, "N/A");
			else gf_fprintf(dump, LLD, ((s64)cts) - ((s64)dts) );
		}

		else if (!strcmp(key, "dur")) gf_fprintf(dump, "%u", gf_filter_pck_get_duration(pck) );
		else if (!strcmp(key, "frame")) {
			Bool start, end;
			gf_filter_pck_get_framing(pck, &start, &end);
			if (start && end) gf_fprintf(dump, "frame_full");
			else if (start) gf_fprintf(dump, "frame_start");
			else if (end) gf_fprintf(dump, "frame_end");
			else gf_fprintf(dump, "frame_cont");
		}
		else if (!strcmp(key, "sap") || !strcmp(key, "rap")) gf_fprintf(dump, "%u", gf_filter_pck_get_sap(pck) );
		else if (!strcmp(key, "ilace")) gf_fprintf(dump, "%d", gf_filter_pck_get_interlaced(pck) );
		else if (!strcmp(key, "corr")) gf_fprintf(dump, "%d", gf_filter_pck_get_corrupted(pck) );
		else if (!strcmp(key, "seek")) gf_fprintf(dump, "%d", gf_filter_pck_get_seek_flag(pck) );
		else if (!strcmp(key, "bo")) {
			u64 bo = gf_filter_pck_get_byte_offset(pck);
			if (bo==GF_FILTER_NO_BO) gf_fprintf(dump, "N/A");
			else gf_fprintf(dump, LLU, bo);
		}
		else if (!strcmp(key, "roll")) gf_fprintf(dump, "%d", gf_filter_pck_get_roll_info(pck) );
		else if (!strcmp(key, "crypt")) gf_fprintf(dump, "%d", gf_filter_pck_get_crypt_flags(pck) );
		else if (!strcmp(key, "vers")) gf_fprintf(dump, "%d", gf_filter_pck_get_carousel_version(pck) );
		else if (!strcmp(key, "size")) gf_fprintf(dump, "%d", size );
		else if (!strcmp(key, "crc")) gf_fprintf(dump, "0x%08X", gf_crc_32(data, size) );
		else if (!strcmp(key, "lf")) gf_fprintf(dump, "\n" );
		else if (!strcmp(key, "cr")) gf_fprintf(dump, "\r" );
		else if (!strcmp(key, "data")) {
			u32 i;
			for (i=0; i<size; i++) {
				gf_fprintf(dump, "%02X", (unsigned char) data[i]);
			}
		}
		else if (!strcmp(key, "lp")) {
			u8 flags = gf_filter_pck_get_dependency_flags(pck);
			flags >>= 6;
			gf_fprintf(dump, "%u", flags);
		}
		else if (!strcmp(key, "depo")) {
			u8 flags = gf_filter_pck_get_dependency_flags(pck);
			flags >>= 4;
			flags &= 0x3;
			gf_fprintf(dump, "%u", flags);
		}
		else if (!strcmp(key, "depf")) {
			u8 flags = gf_filter_pck_get_dependency_flags(pck);
			flags >>= 2;
			flags &= 0x3;
			gf_fprintf(dump, "%u", flags);
		}
		else if (!strcmp(key, "red")) {
			u8 flags = gf_filter_pck_get_dependency_flags(pck);
			flags &= 0x3;
			gf_fprintf(dump, "%u", flags);
		}
		else if (!strcmp(key, "ck")) gf_fprintf(dump, "%d", gf_filter_pck_get_clock_type(pck) );
		else if (!strncmp(key, "pid.", 4)) {
			const GF_PropertyValue *prop = NULL;
			u32 prop_4cc=0;
			const char *pkey = key+4;
			prop_4cc = gf_props_get_id(pkey);
			if (!prop_4cc && strlen(pkey)==4) prop_4cc = GF_4CC(pkey[0],pkey[1],pkey[2],pkey[3]);

			if (prop_4cc) {
				prop = gf_filter_pid_get_property(pctx->src_pid, prop_4cc);
			}
			if (!prop) prop = gf_filter_pid_get_property_str(pctx->src_pid, key);

			if (prop) {
				gf_fprintf(dump, "%s", gf_props_dump(prop_4cc, prop, szDump, ctx->dump_data) );
			}
		}
		else {
			const GF_PropertyValue *prop = NULL;
			u32 prop_4cc = gf_props_get_id(key);
			if (!prop_4cc && strlen(key)==4) prop_4cc = GF_4CC(key[0],key[1],key[2],key[3]);

			if (prop_4cc) {
				prop = gf_filter_pck_get_property(pck, prop_4cc);
			}
			if (!prop) prop = gf_filter_pck_get_property_str(pck, key);

			if (prop) {
				gf_fprintf(dump, "%s", gf_props_dump(prop_4cc, prop, szDump, ctx->dump_data) );
			}
		}

		sep[0] = csep;
		str = sep+1;
	}
}

void gf_m4v_parser_set_inspect(GF_M4VParser *m4v);
u32 gf_m4v_parser_get_obj_type(GF_M4VParser *m4v);

#ifndef GPAC_DISABLE_AV_PARSERS
static void inspect_dump_mpeg124(PidCtx *pctx, char *data, u32 size, FILE *dump)
{
	u8 ftype;
	u32 tinc, o_type;
	u64 fsize, start;
	Bool is_coded, is_m4v=(pctx->codec_id==GF_CODECID_MPEG4_PART2) ? GF_TRUE : GF_FALSE;
	GF_Err e;
	GF_M4VParser *m4v = gf_m4v_parser_new(data, size, !is_m4v);

	gf_m4v_parser_set_inspect(m4v);
	while (1) {
		ftype = 0;
		is_coded = GF_FALSE;
		e = gf_m4v_parse_frame(m4v, &pctx->dsi, &ftype, &tinc, &fsize, &start, &is_coded);
		if (e)
			break;

		o_type = gf_m4v_parser_get_obj_type(m4v);
		if (is_m4v) {
			gf_fprintf(dump, "<MPEG4P2VideoObj type=\"0x%02X\"", o_type);
			switch (o_type) {
			/*vosh*/
			case M4V_VOS_START_CODE:
				gf_fprintf(dump, " name=\"VOS\" PL=\"%d\"", pctx->dsi.VideoPL);
				break;
			case M4V_VOL_START_CODE:
				gf_fprintf(dump, " name=\"VOL\" RAP=\"%d\" objectType=\"%d\" par=\"%d/%d\" hasShape=\"%d\"", pctx->dsi.RAP_stream, pctx->dsi.objectType, pctx->dsi.par_num, pctx->dsi.par_den, pctx->dsi.has_shape);
				if (pctx->dsi.clock_rate)
					gf_fprintf(dump, " clockRate=\"%d\"", pctx->dsi.clock_rate);
				if (pctx->dsi.time_increment)
					gf_fprintf(dump, " timeIncrement=\"%d\"", pctx->dsi.time_increment);
				if (!pctx->dsi.has_shape)
					gf_fprintf(dump, " width=\"%d\" height=\"%d\"", pctx->dsi.width, pctx->dsi.height);

				break;

			case M4V_VOP_START_CODE:
				gf_fprintf(dump, " name=\"VOP\" frameType=\"%d\" timeInc=\"%d\" isCoded=\"%d\"", ftype, tinc, is_coded);
				break;
			case M4V_GOV_START_CODE:
				gf_fprintf(dump, " name=\"GOV\"");
				break;
			/*don't interest us*/
			case M4V_UDTA_START_CODE:
				gf_fprintf(dump, " name=\"UDTA\"");
				break;

	 		case M4V_VO_START_CODE:
				gf_fprintf(dump, " name=\"VO\"");
				break;
	 		case M4V_VISOBJ_START_CODE:
				gf_fprintf(dump, " name=\"VisObj\"");
				break;
			default:
				break;
			}
			gf_fprintf(dump, "/>\n");
		} else {
			gf_fprintf(dump, "<MPEG12VideoObj type=\"0x%02X\"", o_type);
			switch (o_type) {
			case M2V_SEQ_START_CODE:
				gf_fprintf(dump, " name=\"SeqStart\" width=\"%d\" height=\"%d\" sar=\"%d/%d\" fps=\"%f\"", pctx->dsi.width, pctx->dsi.height, pctx->dsi.par_num, pctx->dsi.par_den, pctx->dsi.fps);
				break;

			case M2V_EXT_START_CODE:
				gf_fprintf(dump, " name=\"SeqStartEXT\" width=\"%d\" height=\"%d\" PL=\"%d\"", pctx->dsi.width, pctx->dsi.height, pctx->dsi.VideoPL);
				break;
			case M2V_PIC_START_CODE:
				gf_fprintf(dump, " name=\"PicStart\" frameType=\"%d\" isCoded=\"%d\"", ftype, is_coded);
				break;
			case M2V_GOP_START_CODE:
				gf_fprintf(dump, " name=\"GOPStart\"");
				break;
			default:
				break;
			}
			gf_fprintf(dump, "/>\n");
		}
	}
	gf_m4v_parser_del(m4v);
}
#endif

static void inspect_dump_tmcd(GF_InspectCtx *ctx, PidCtx *pctx, char *data, u32 size, FILE *dump)
{
	u32 h, m, s, f;
	Bool neg=GF_FALSE;
	GF_BitStream *bs;
	if (!pctx->tmcd_rate.den || !pctx->tmcd_rate.num)
		return;
	bs = gf_bs_new(data, size, GF_BITSTREAM_READ);

	u32 value = gf_bs_read_u32(bs);
	gf_bs_seek(bs, 0);
	gf_fprintf(dump, "<TimeCode");

	if (ctx->fftmcd || (pctx->tmcd_flags & 0x00000008)) {
		u64 nb_secs, nb_frames = value;
		Bool is_drop = GF_FALSE;
		if (!ctx->fftmcd && pctx->tmcd_fpt)
			nb_frames *= pctx->tmcd_fpt;

		if (ctx->fftmcd) {
			u32 fps = pctx->tmcd_rate.num / pctx->tmcd_rate.den;
			if (fps * pctx->tmcd_rate.den != pctx->tmcd_rate.num)
				is_drop = GF_TRUE;
		}
		else if (pctx->tmcd_flags & 0x00000001) {
			is_drop = GF_TRUE;
		}

		if (is_drop) {
			u32 drop_frames, frame_base;

			frame_base = 100 * pctx->tmcd_rate.num;
			frame_base /= pctx->tmcd_rate.den;

			drop_frames = (u32) (nb_frames / frame_base);
			nb_frames -= 3*drop_frames;
		}

		nb_secs = nb_frames * pctx->tmcd_rate.den / pctx->tmcd_rate.num;

		gf_fprintf(dump, " counter=\"%d\"", value);
		h = (u32) nb_secs/3600;
		m = (u32) (nb_secs/60 - h*60);
		s = (u32) (nb_secs - m*60 - h*3600);

		nb_secs *= pctx->tmcd_rate.num;
		nb_secs /= pctx->tmcd_rate.den;
		f = (u32) (nb_frames - nb_secs);
		if (pctx->tmcd_fpt && (f==pctx->tmcd_fpt)) {
			f = 0;
			s++;
			if (s==60) {
				s = 0;
				m++;
				if (m==60) {
					m = 0;
					h++;
				}
			}
		}
	} else {
		h = gf_bs_read_u8(bs);
		neg = gf_bs_read_int(bs, 1);
		m = gf_bs_read_int(bs, 7);
		s = gf_bs_read_u8(bs);
		f = gf_bs_read_u8(bs);
	}
	gf_fprintf(dump, " time=\"%s%02d:%02d:%02d:%02d\"/>\n", neg ? "-" : "", h, m, s, f);
	gf_bs_del(bs);


}

static void inspect_dump_packet(GF_InspectCtx *ctx, FILE *dump, GF_FilterPacket *pck, u32 pid_idx, u64 pck_num, PidCtx *pctx)
{
	u32 idx=0, size;
	u64 ts;
	u8 dflags = 0;
	GF_FilterClockType ck_type;
	GF_FilterFrameInterface *fifce=NULL;
	Bool start, end;
	u8 *data;

	if (!ctx->deep && !ctx->fmt) return;

	data = (u8 *) gf_filter_pck_get_data(pck, &size);
	gf_filter_pck_get_framing(pck, &start, &end);

	ck_type = ctx->pcr ? gf_filter_pck_get_clock_type(pck) : 0;
	if (!size && !ck_type) {
		fifce = gf_filter_pck_get_frame_interface(pck);
		if (!fifce) return;
	}

	if (ctx->xml) {
		gf_fprintf(dump, "<Packet number=\""LLU"\"", pck_num);
		if (ctx->interleave)
			gf_fprintf(dump, " PID=\"%d\"", pid_idx);
	} else {
		gf_fprintf(dump, "PID %d PCK "LLU" - ", pid_idx, pck_num);
	}
	if (ck_type) {
		ts = gf_filter_pck_get_cts(pck);
		if (ctx->xml) {
			if (ts==GF_FILTER_NO_TS) gf_fprintf(dump, " PCR=\"N/A\"");
			else gf_fprintf(dump, " PCR=\""LLU"\" ", ts );
			if (ck_type!=GF_FILTER_CLOCK_PCR) gf_fprintf(dump, " discontinuity=\"true\"");
			gf_fprintf(dump, "/>");
		} else {
			if (ts==GF_FILTER_NO_TS) gf_fprintf(dump, " PCR N/A");
			else gf_fprintf(dump, " PCR%s "LLU"\n", (ck_type==GF_FILTER_CLOCK_PCR) ? "" : " discontinuity", ts );
		}
		return;
	}
	if (ctx->xml) {
		if (fifce) gf_fprintf(dump, " framing=\"interface\"");
		else if (start && end) gf_fprintf(dump, " framing=\"complete\"");
		else if (start) gf_fprintf(dump, " framing=\"start\"");
		else if (end) gf_fprintf(dump, " framing=\"end\"");
		else gf_fprintf(dump, " framing=\"continuation\"");
	} else {
		if (fifce) gf_fprintf(dump, "interface");
		else if (start && end) gf_fprintf(dump, "full frame");
		else if (start) gf_fprintf(dump, "frame start");
		else if (end) gf_fprintf(dump, "frame end");
		else gf_fprintf(dump, "frame continuation");
	}
	ts = gf_filter_pck_get_dts(pck);
	if (ts==GF_FILTER_NO_TS) DUMP_ATT_STR("dts", "N/A")
	else DUMP_ATT_LLU("dts", ts )

	ts = gf_filter_pck_get_cts(pck);
	if (ts==GF_FILTER_NO_TS) DUMP_ATT_STR("cts", "N/A")
	else DUMP_ATT_LLU("cts", ts )

	DUMP_ATT_U("dur", gf_filter_pck_get_duration(pck) )
	DUMP_ATT_U("sap", gf_filter_pck_get_sap(pck) )
	DUMP_ATT_D("ilace", gf_filter_pck_get_interlaced(pck) )
	DUMP_ATT_D("corr", gf_filter_pck_get_corrupted(pck) )
	DUMP_ATT_D("seek", gf_filter_pck_get_seek_flag(pck) )

	ts = gf_filter_pck_get_byte_offset(pck);
	if (ts==GF_FILTER_NO_BO) DUMP_ATT_STR("bo", "N/A")
	else DUMP_ATT_LLU("bo", ts )

	DUMP_ATT_U("roll", gf_filter_pck_get_roll_info(pck) )
	DUMP_ATT_U("crypt", gf_filter_pck_get_crypt_flags(pck) )
	DUMP_ATT_U("vers", gf_filter_pck_get_carousel_version(pck) )

	if (!ck_type && !fifce) {
		DUMP_ATT_U("size", size )
	}
	dflags = gf_filter_pck_get_dependency_flags(pck);
	DUMP_ATT_U("lp", (dflags>>6) & 0x3 )
	DUMP_ATT_U("depo", (dflags>>4) & 0x3 )
	DUMP_ATT_U("depf", (dflags>>2) & 0x3 )
	DUMP_ATT_U("red", (dflags) & 0x3 )

	if (ctx->dump_data) {
		u32 i;
		DUMP_ATT_STR("data", "")
		for (i=0; i<size; i++) {
			gf_fprintf(dump, "%02X", (unsigned char) data[i]);
		}
		if (ctx->xml) gf_fprintf(dump, "\"");
	} else if (fifce) {
		u32 i;
		char *name = fifce->get_gl_texture ? "Interface_GLTexID" : "Interface_NumPlanes";
		if (ctx->xml) {
			gf_fprintf(dump, " %s=\"", name);
		} else {
			gf_fprintf(dump, " %s ", name);
		}
		for (i=0; i<4; i++) {
			if (fifce->get_gl_texture) {
				u32 gl_tex_format, gl_tex_id;
				if (fifce->get_gl_texture(fifce, i, &gl_tex_format, &gl_tex_id, NULL) != GF_OK)
					break;
				if (i) gf_fprintf(dump, ",");
				gf_fprintf(dump, "%d", gl_tex_id);
			} else {
				u32 stride;
				const u8 *plane;
				if (fifce->get_plane(fifce, i, &plane, &stride) != GF_OK)
					break;
			}
		}
		if (!fifce->get_gl_texture) {
			gf_fprintf(dump, "%d", i);
		}

		if (ctx->xml) {
			gf_fprintf(dump, "\"");
		}
	} else if (data) {
		DUMP_ATT_X("CRC32", gf_crc_32(data, size) )
	}
	if (ctx->xml) {
		if (!ctx->props) goto props_done;

	} else {
		gf_fprintf(dump, "\n");
	}

	if (!ctx->props) return;
	while (1) {
		u32 prop_4cc;
		const char *prop_name;
		const GF_PropertyValue * p = gf_filter_pck_enum_properties(pck, &idx, &prop_4cc, &prop_name);
		if (!p) break;
		if (idx==0) gf_fprintf(dump, "properties:\n");

		inspect_dump_property(ctx, dump, prop_4cc, prop_name, p);
	}


props_done:
	if (!ctx->analyze) {
		if (ctx->xml) {
			gf_fprintf(dump, "/>\n");
		}
		return;
	}
	gf_fprintf(dump, ">\n");

#ifndef GPAC_DISABLE_AV_PARSERS
	if (pctx->hevc_state || pctx->avc_state) {
		idx=1;

		if (pctx->is_adobe_protected) {
			u8 encrypted_au = data[0];
			if (encrypted_au) {
				gf_fprintf(dump, "   <!-- Packet is an Adobe's protected frame and can not be dumped -->\n");
				gf_fprintf(dump, "</Packet>\n");
				return;
			}
			else {
				data++;
				size--;
			}
		}
		while (size) {
			u32 nal_size = inspect_get_nal_size((char*)data, pctx->nalu_size_length);
			data += pctx->nalu_size_length;

			if (pctx->nalu_size_length + nal_size > size) {
				gf_fprintf(dump, "   <!-- NALU is corrupted: size is %d but only %d remains -->\n", nal_size, size);
				break;
			} else {
				gf_fprintf(dump, "   <NALU size=\"%d\" ", nal_size);
				gf_inspect_dump_nalu(dump, data, nal_size, pctx->has_svcc ? 1 : 0, pctx->hevc_state, pctx->avc_state, pctx->nalu_size_length, ctx->dump_crc, pctx->is_cenc_protected);
				gf_fprintf(dump, "/>\n");
			}
			idx++;
			data += nal_size;
			size -= nal_size + pctx->nalu_size_length;
		}
	} else if (pctx->av1_state) {
		GF_BitStream *bs = gf_bs_new(data, size, GF_BITSTREAM_READ);
		while (size) {
			ObuType obu_type;
			u64 obu_size;
			u32 hdr_size;
			gf_media_aom_av1_parse_obu(bs, &obu_type, &obu_size, &hdr_size, pctx->av1_state);

			if (obu_size > size) {
				gf_fprintf(dump, "   <!-- OBU is corrupted: size is %d but only %d remains -->\n", (u32) obu_size, size);
				break;
			}

			gf_inspect_dump_obu(dump, pctx->av1_state, (char *) data, obu_size, obu_type, obu_size, hdr_size, ctx->dump_crc);
			data += obu_size;
			size -= (u32)obu_size;
			idx++;
		}
		gf_bs_del(bs);
	} else {
		u32 hdr, pos, fsize, i;
		switch (pctx->codec_id) {
		case GF_CODECID_MPEG1:
		case GF_CODECID_MPEG2_422:
		case GF_CODECID_MPEG2_SNR:
		case GF_CODECID_MPEG2_HIGH:
		case GF_CODECID_MPEG2_MAIN:
		case GF_CODECID_MPEG4_PART2:
			inspect_dump_mpeg124(pctx, (char *) data, size, dump);
			break;
		case GF_CODECID_MPEG_AUDIO:
		case GF_CODECID_MPEG2_PART3:
			pos = 0;
			while (size) {
				hdr = gf_mp3_get_next_header_mem(data, size, &pos);
				fsize = gf_mp3_frame_size(hdr);
				gf_fprintf(dump, "<MPEGAudioFrame size=\"%d\" layer=\"%d\" version=\"%d\" bitrate=\"%d\" channels=\"%d\" samplesPerFrame=\"%d\" samplerate=\"%d\"/>\n", fsize, gf_mp3_layer(hdr), gf_mp3_version(hdr), gf_mp3_bit_rate(hdr), gf_mp3_num_channels(hdr), gf_mp3_window_size(hdr), gf_mp3_sampling_rate(hdr));
				if (size<pos+fsize) break;
				data += pos + fsize;
				size -= pos + fsize;
			}
			break;
		case GF_CODECID_TMCD:
			inspect_dump_tmcd(ctx, pctx, (char *) data, size, dump);
			break;
		case GF_CODECID_SUBS_TEXT:
		case GF_CODECID_META_TEXT:
			gf_fprintf(dump, "<![CDATA[");
			for (i=0; i<size; i++) {
				gf_fputc(data[i], dump);
			}
			gf_fprintf(dump, "]]>\n");
			break;
		case GF_CODECID_SUBS_XML:
		case GF_CODECID_META_XML:
			for (i=0; i<size; i++) {
				gf_fputc(data[i], dump);
			}
			break;
		case GF_CODECID_APCH:
		case GF_CODECID_APCO:
		case GF_CODECID_APCN:
		case GF_CODECID_APCS:
		case GF_CODECID_AP4X:
		case GF_CODECID_AP4H:
			gf_inspect_dump_prores(dump, (char *) data, size, ctx->dump_crc);
			break;

		}
	}
#endif
	gf_fprintf(dump, "</Packet>\n");
}

#define DUMP_ARRAY(arr, name, loc, _is_svc)\
	if (arr && gf_list_count(arr)) {\
		gf_fprintf(dump, "  <%sArray location=\"%s\">\n", name, loc);\
		for (i=0; i<gf_list_count(arr); i++) {\
			slc = gf_list_get(arr, i);\
			gf_fprintf(dump, "   <NALU size=\"%d\" ", slc->size);\
			gf_inspect_dump_nalu(dump, slc->data, slc->size, _is_svc, pctx->hevc_state, pctx->avc_state, nalh_size, ctx->dump_crc, GF_FALSE);\
			gf_fprintf(dump, "/>\n");\
		}\
		gf_fprintf(dump, "  </%sArray>\n", name);\
	}\


#ifndef GPAC_DISABLE_AV_PARSERS
static void inspect_reset_parsers(PidCtx *pctx, void *keep_parser_address)
{
	if ((&pctx->hevc_state != keep_parser_address) && pctx->hevc_state) {
		gf_free(pctx->hevc_state);
		pctx->hevc_state = NULL;
	}
	if ((&pctx->avc_state != keep_parser_address) && pctx->avc_state) {
		gf_free(pctx->avc_state);
		pctx->avc_state = NULL;
	}
	if ((&pctx->av1_state != keep_parser_address) && pctx->av1_state) {
		if (pctx->av1_state->config) gf_odf_av1_cfg_del(pctx->av1_state->config);
		gf_free(pctx->av1_state);
		pctx->av1_state = NULL;
	}
	if ((&pctx->mv124_state != keep_parser_address) && pctx->mv124_state) {
		gf_m4v_parser_del(pctx->mv124_state);
		pctx->mv124_state = NULL;
	}
}
#endif

static void inspect_dump_pid(GF_InspectCtx *ctx, FILE *dump, GF_FilterPid *pid, u32 pid_idx, Bool is_connect, Bool is_remove, u64 pck_for_config, Bool is_info, PidCtx *pctx)
{
	u32 idx=0, nalh_size;
#ifndef GPAC_DISABLE_AV_PARSERS
	u32 i;
	GF_AVCConfigSlot *slc;
#endif
	GF_AVCConfig *avcc, *svcc;
	GF_HEVCConfig *hvcc, *lhcc;
	Bool is_enh = GF_FALSE;
	Bool is_svc=GF_FALSE;
	char *elt_name = NULL;
	const GF_PropertyValue *p, *dsi, *dsi_enh;

	if (ctx->test==INSPECT_TEST_NOPROP) return;

	//disconnect of src pid (not yet supported)
	if (ctx->xml) {
		if (is_info) {
			elt_name = "PIDInfoUpdate";
		} else if (is_remove) {
			elt_name = "PIDRemove";
		} else {
			elt_name = is_connect ? "PIDConfigure" : "PIDReconfigure";
		}
		gf_fprintf(dump, "<%s PID=\"%d\" name=\"%s\"",elt_name, pid_idx, gf_filter_pid_get_name(pid) );

		if (pck_for_config)
			gf_fprintf(dump, " packetsSinceLastConfig=\""LLU"\"", pck_for_config);
	} else {
		if (is_info) {
			gf_fprintf(dump, "PID %d name %s info update\n", pid_idx, gf_filter_pid_get_name(pid) );
		} else if (is_remove) {
			gf_fprintf(dump, "PID %d name %s delete\n", pid_idx, gf_filter_pid_get_name(pid) );
		} else {
			gf_fprintf(dump, "PID %d name %s %sonfigure", pid_idx, gf_filter_pid_get_name(pid), is_connect ? "C" : "Rec" );
			if (pck_for_config)
				gf_fprintf(dump, " after "LLU" packets", pck_for_config);
			gf_fprintf(dump, " - properties:\n");
		}
	}

	if (!is_info) {
		while (1) {
			u32 prop_4cc;
			const char *prop_name;
			p = gf_filter_pid_enum_properties(pid, &idx, &prop_4cc, &prop_name);
			if (!p) break;
			inspect_dump_property(ctx, dump, prop_4cc, prop_name, p);

			if (prop_name) {
				if (!strcmp(prop_name, "tmcd:flags"))
					pctx->tmcd_flags = p->value.uint;
				else if (!strcmp(prop_name, "tmcd:framerate"))
					pctx->tmcd_rate = p->value.frac;
				else if (!strcmp(prop_name, "tmcd:frames_per_tick"))
					pctx->tmcd_fpt = p->value.uint;

			}
		}
	} else {
		while (ctx->info) {
			u32 prop_4cc;
			const char *prop_name;
			p = gf_filter_pid_enum_info(pid, &idx, &prop_4cc, &prop_name);
			if (!p) break;
			inspect_dump_property(ctx, dump, prop_4cc, prop_name, p);
		}
	}

	if (!ctx->analyze) {
		if (ctx->xml) {
			gf_fprintf(dump, "/>\n");
		}
		return;
	}

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_CODECID);
	if (!p) {
		gf_fprintf(dump, "/>\n");
		return;
	}
	pctx->codec_id = p->value.uint;

	dsi = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG);
	dsi_enh = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG_ENHANCEMENT);

	avcc = NULL;
	svcc = NULL;
	hvcc = NULL;
	lhcc = NULL;
	pctx->has_svcc = 0;

	switch (pctx->codec_id) {
	case GF_CODECID_SVC:
	case GF_CODECID_MVC:
		if (!dsi) is_enh = GF_TRUE;
	case GF_CODECID_AVC:
	case GF_CODECID_AVC_PS:
		if (!dsi && !dsi_enh) {
			gf_fprintf(dump, "/>\n");
			return;
		}
#ifndef GPAC_DISABLE_AV_PARSERS
		inspect_reset_parsers(pctx, &pctx->avc_state);

		if (!pctx->avc_state) {
			GF_SAFEALLOC(pctx->avc_state, AVCState);
		}
#endif
		gf_fprintf(dump, ">\n");
		gf_fprintf(dump, "<AVCParameterSets>\n");
		if (dsi) {
			if (is_enh) {
				svcc = gf_odf_avc_cfg_read(dsi->value.data.ptr, dsi->value.data.size);
				if (svcc)
					pctx->nalu_size_length = svcc->nal_unit_size;
			} else {
				avcc = gf_odf_avc_cfg_read(dsi->value.data.ptr, dsi->value.data.size);
				if (avcc)
					pctx->nalu_size_length = avcc->nal_unit_size;
			}
		}
		if (dsi_enh && !svcc) {
			svcc = gf_odf_avc_cfg_read(dsi_enh->value.data.ptr, dsi_enh->value.data.size);
			if (svcc)
				pctx->nalu_size_length = svcc->nal_unit_size;
		}
		nalh_size = pctx->nalu_size_length;
		is_svc = (svcc != NULL) ? 1 : 0;
#ifndef GPAC_DISABLE_AV_PARSERS
		if (avcc) {
			DUMP_ARRAY(avcc->sequenceParameterSets, "AVCSPS", "decoderConfig", is_svc)
			DUMP_ARRAY(avcc->pictureParameterSets, "AVCPPS", "decoderConfig", is_svc)
			DUMP_ARRAY(avcc->sequenceParameterSetExtensions, "AVCSPSEx", "decoderConfig", is_svc)
		}
		if (is_svc) {
			DUMP_ARRAY(svcc->sequenceParameterSets, "SVCSPS", dsi_enh ? "decoderConfigEnhancement" : "decoderConfig", is_svc)
			DUMP_ARRAY(svcc->pictureParameterSets, "SVCPPS", dsi_enh ? "decoderConfigEnhancement" : "decoderConfig", is_svc)
			pctx->has_svcc = 1;
		}
#endif
		gf_fprintf(dump, "</AVCParameterSets>\n");
		break;
	case GF_CODECID_LHVC:
		is_enh = GF_TRUE;
	case GF_CODECID_HEVC:
	case GF_CODECID_HEVC_TILES:
		if (!dsi && !dsi_enh) {
			gf_fprintf(dump, "/>\n");
			return;
		}

#ifndef GPAC_DISABLE_AV_PARSERS
		inspect_reset_parsers(pctx, &pctx->hevc_state);

		if (!pctx->hevc_state) {
			GF_SAFEALLOC(pctx->hevc_state, HEVCState);
		}
#endif

		if (dsi) {
			if (is_enh && !dsi_enh) {
				lhcc = gf_odf_hevc_cfg_read(dsi->value.data.ptr, dsi->value.data.size, GF_TRUE);
				if (lhcc)
					pctx->nalu_size_length = lhcc->nal_unit_size;
			} else {
				hvcc = gf_odf_hevc_cfg_read(dsi->value.data.ptr, dsi->value.data.size, GF_FALSE);
				if (hvcc)
					pctx->nalu_size_length = hvcc->nal_unit_size;
			}
		}
		if (dsi_enh && !lhcc) {
			lhcc = gf_odf_hevc_cfg_read(dsi_enh->value.data.ptr, dsi_enh->value.data.size, GF_TRUE);
			if (lhcc)
				pctx->nalu_size_length = lhcc->nal_unit_size;
		}
		nalh_size = pctx->nalu_size_length;

		gf_fprintf(dump, ">\n");
		gf_fprintf(dump, "<HEVCParameterSets>\n");
#ifndef GPAC_DISABLE_AV_PARSERS
		if (hvcc) {
			for (idx=0; idx<gf_list_count(hvcc->param_array); idx++) {
				GF_HEVCParamArray *ar = gf_list_get(hvcc->param_array, idx);
				if (ar->type==GF_HEVC_NALU_SEQ_PARAM) {
					DUMP_ARRAY(ar->nalus, "HEVCSPS", "hvcC", 0)
				} else if (ar->type==GF_HEVC_NALU_PIC_PARAM) {
					DUMP_ARRAY(ar->nalus, "HEVCPPS", "hvcC", 0)
				} else if (ar->type==GF_HEVC_NALU_VID_PARAM) {
					DUMP_ARRAY(ar->nalus, "HEVCVPS", "hvcC", 0)
				} else {
					DUMP_ARRAY(ar->nalus, "HEVCUnknownPS", "hvcC", 0)
				}
			}
		}
		if (lhcc) {
			for (idx=0; idx<gf_list_count(lhcc->param_array); idx++) {
				GF_HEVCParamArray *ar = gf_list_get(lhcc->param_array, idx);
				if (ar->type==GF_HEVC_NALU_SEQ_PARAM) {
					DUMP_ARRAY(ar->nalus, "HEVCSPS", "lhcC", 0)
				} else if (ar->type==GF_HEVC_NALU_PIC_PARAM) {
					DUMP_ARRAY(ar->nalus, "HEVCPPS", "lhcC", 0)
				} else if (ar->type==GF_HEVC_NALU_VID_PARAM) {
					DUMP_ARRAY(ar->nalus, "HEVCVPS", "lhcC", 0)
				} else {
					DUMP_ARRAY(ar->nalus, "HEVCUnknownPS", "lhcC", 0)
				}
			}
		}
#endif
		gf_fprintf(dump, "</HEVCParameterSets>\n");
		break;

	case GF_CODECID_AV1:
#ifndef GPAC_DISABLE_AV_PARSERS
		inspect_reset_parsers(pctx, &pctx->av1_state);
		if (!pctx->av1_state) {
			GF_SAFEALLOC(pctx->av1_state, AV1State);
		}
#endif
		if (!dsi) {
			gf_fprintf(dump, "/>\n");
			return;
		}
#ifndef GPAC_DISABLE_AV_PARSERS
		if (pctx->av1_state->config) gf_odf_av1_cfg_del(pctx->av1_state->config);
		pctx->av1_state->config = gf_odf_av1_cfg_read(dsi->value.data.ptr, dsi->value.data.size);
		gf_fprintf(dump, ">\n");
		gf_fprintf(dump, " <OBUConfig>\n");

		idx = 1;
		for (i=0; i<gf_list_count(pctx->av1_state->config->obu_array); i++) {
			ObuType obu_type;
			u64 obu_size;
			u32 hdr_size;
			GF_AV1_OBUArrayEntry *obu = gf_list_get(pctx->av1_state->config->obu_array, i);
			GF_BitStream *bs = gf_bs_new((const char *)obu->obu, (u32) obu->obu_length, GF_BITSTREAM_READ);
			gf_media_aom_av1_parse_obu(bs, &obu_type, &obu_size, &hdr_size, pctx->av1_state);
			gf_inspect_dump_obu(dump, pctx->av1_state, (char*)obu->obu, obu->obu_length, obu_type, obu_size, hdr_size, ctx->dump_crc);
			gf_bs_del(bs);
			idx++;
		}
#endif
		gf_fprintf(dump, " </OBUConfig>\n");
		break;

	case GF_CODECID_MPEG1:
	case GF_CODECID_MPEG2_422:
	case GF_CODECID_MPEG2_SNR:
	case GF_CODECID_MPEG2_HIGH:
	case GF_CODECID_MPEG2_MAIN:
	case GF_CODECID_MPEG4_PART2:

#ifndef GPAC_DISABLE_AV_PARSERS
		inspect_reset_parsers(pctx, &pctx->mv124_state);
#endif
		if (!dsi) {
			gf_fprintf(dump, "/>\n");
			return;
		}
		gf_fprintf(dump, ">\n");
		gf_fprintf(dump, " <MPEGVideoConfig>\n");
#ifndef GPAC_DISABLE_AV_PARSERS
		inspect_dump_mpeg124(pctx, dsi->value.data.ptr, dsi->value.data.size, dump);
#endif
		gf_fprintf(dump, " </MPEGVideoConfig>\n");
#ifndef GPAC_DISABLE_AV_PARSERS
		if (pctx->mv124_state) gf_m4v_parser_del(pctx->mv124_state);
#endif
		break;
	case GF_CODECID_MPEG_AUDIO:
	case GF_CODECID_MPEG2_PART3:
	case GF_CODECID_TMCD:
		gf_fprintf(dump, "/>\n");
		return;
	case GF_CODECID_SUBS_XML:
	case GF_CODECID_META_XML:
		if (!dsi) {
			gf_fprintf(dump, "/>\n");
			return;
		}
		gf_fprintf(dump, " <XMLTextConfig>\n");
		for (i=0; i<dsi->value.data.size; i++) {
			gf_fputc(dsi->value.data.ptr[i], dump);
		}
		gf_fprintf(dump, "\n </XMLTextConfig>\n");
		gf_fprintf(dump, "/>\n");
		return;
	case GF_CODECID_SUBS_TEXT:
	case GF_CODECID_META_TEXT:
		if (!dsi) {
			gf_fprintf(dump, "/>\n");
			return;
		}
		gf_fprintf(dump, " <TextConfig>\n");
		gf_fprintf(dump, "<![CDATA[");
		for (i=0; i<dsi->value.data.size; i++) {
			gf_fputc(dsi->value.data.ptr[i], dump);
		}
		gf_fprintf(dump, "]]>\n");
		gf_fprintf(dump, " </TextConfig>\n");
		gf_fprintf(dump, "/>\n");
		return;
	case GF_CODECID_APCH:
	case GF_CODECID_APCN:
	case GF_CODECID_APCS:
	case GF_CODECID_APCO:
	case GF_CODECID_AP4H:
	case GF_CODECID_AP4X:
		gf_fprintf(dump, "/>\n");
		return;
	default:
		GF_LOG(GF_LOG_WARNING, GF_LOG_AUTHOR, ("[Inspect] bitstream analysis for codec %s not supported\n", gf_codecid_name(pctx->codec_id)));
		gf_fprintf(dump, "/>\n");
		return;
	}

	if (avcc) gf_odf_avc_cfg_del(avcc);
	if (svcc) gf_odf_avc_cfg_del(svcc);
	if (hvcc) gf_odf_hevc_cfg_del(hvcc);
	if (lhcc) gf_odf_hevc_cfg_del(lhcc);

	gf_fprintf(dump, "</%s>\n", elt_name);
}

static GF_Err inspect_process(GF_Filter *filter)
{
	u32 i, count, nb_done=0;
	GF_InspectCtx *ctx = (GF_InspectCtx *) gf_filter_get_udta(filter);

	count = gf_list_count(ctx->src_pids);
	for (i=0; i<count; i++) {
		PidCtx *pctx = gf_list_get(ctx->src_pids, i);
		GF_FilterPacket *pck = gf_filter_pid_get_packet(pctx->src_pid);

		if (!pck && !gf_filter_pid_is_eos(pctx->src_pid))
			continue;

		if (pctx->dump_pid) {
			inspect_dump_pid(ctx, pctx->tmp, pctx->src_pid, pctx->idx, pctx->init_pid_config_done ? GF_FALSE : GF_TRUE, GF_FALSE, pctx->pck_for_config, (pctx->dump_pid==2) ? GF_TRUE : GF_FALSE, pctx);
			pctx->dump_pid = 0;
			pctx->init_pid_config_done = 1;
			pctx->pck_for_config=0;

			if (!ctx->hdr_done) {
				ctx->hdr_done=GF_TRUE;
				//dump header on main output file, not on pid one!
				if (ctx->hdr && ctx->fmt && !ctx->xml)
					inspect_dump_packet_fmt(ctx, ctx->dump, NULL, 0, 0);
			}
		}
		if (!pck) continue;
		
		pctx->pck_for_config++;
		pctx->pck_num++;

		if (ctx->dump_pck) {

			if (ctx->is_prober) {
				nb_done++;
			} else if (ctx->fmt) {
				inspect_dump_packet_fmt(ctx, pctx->tmp, pck, pctx, pctx->pck_num);
			} else {
				inspect_dump_packet(ctx, pctx->tmp, pck, pctx->idx, pctx->pck_num, pctx);
			}
		}
		
		if (ctx->dur.num && ctx->dur.den) {
			u64 timescale = gf_filter_pck_get_timescale(pck);
			u64 ts = gf_filter_pck_get_dts(pck);
			if (ts == GF_FILTER_NO_TS) ts = gf_filter_pck_get_cts(pck);

			if (!pctx->init_ts) pctx->init_ts = ts;
			else if (ctx->dur.den * (ts - pctx->init_ts) >= ctx->dur.num * timescale) {
				GF_FilterEvent evt;
				GF_FEVT_INIT(evt, GF_FEVT_STOP, pctx->src_pid);
				gf_filter_pid_send_event(pctx->src_pid, &evt);
				gf_filter_pid_set_discard(pctx->src_pid, GF_TRUE);
				break;
			}
		}
		gf_filter_pid_drop_packet(pctx->src_pid);
	}
	if (ctx->is_prober && !ctx->probe_done && (nb_done==count) && !ctx->allp) {
		for (i=0; i<count; i++) {
			PidCtx *pctx = gf_list_get(ctx->src_pids, i);
			GF_FilterEvent evt;
			GF_FEVT_INIT(evt, GF_FEVT_STOP, pctx->src_pid);
			gf_filter_pid_send_event(pctx->src_pid, &evt);
		}
		ctx->probe_done = GF_TRUE;
		return GF_EOS;
	}
	return GF_OK;
}

static GF_Err inspect_config_input(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_FilterEvent evt;
	PidCtx *pctx;
	GF_InspectCtx *ctx = (GF_InspectCtx *) gf_filter_get_udta(filter);

	if (!ctx->src_pids) ctx->src_pids = gf_list_new();

	pctx = gf_filter_pid_get_udta(pid);
	if (pctx) {
		assert(pctx->src_pid == pid);
		if (!ctx->is_prober) pctx->dump_pid = 1;
		return GF_OK;
	}
	GF_SAFEALLOC(pctx, PidCtx);
	pctx->src_pid = pid;
	gf_filter_pid_set_udta(pid, pctx);
	if (! ctx->interleave) {
		const GF_PropertyValue *p;

		//in non interleave mode, log audio first and video after - mostly used for tests where we always need th same output
		p = gf_filter_pid_get_property(pid, GF_PROP_PID_STREAM_TYPE);
		if (p && (p->value.uint==GF_STREAM_AUDIO)) {
			u32 i;
			gf_list_insert(ctx->src_pids, pctx, 0);

			for (i=0; i<gf_list_count(ctx->src_pids); i++) {
				PidCtx *actx = gf_list_get(ctx->src_pids, i);
				actx->idx = i+1;
			}
		} else {
			gf_list_add(ctx->src_pids, pctx);
		}
	} else {
		pctx->tmp = ctx->dump;
		gf_list_add(ctx->src_pids, pctx);
	}

	pctx->idx = gf_list_find(ctx->src_pids, pctx) + 1;

	if (! ctx->interleave && !pctx->tmp) {
		pctx->tmp = gf_file_temp(NULL);
		if (ctx->xml)
			gf_fprintf(ctx->dump, "<PIDInspect ID=\"%d\" name=\"%s\">\n", pctx->idx, gf_filter_pid_get_name(pid) );
	}

	switch (ctx->mode) {
	case INSPECT_MODE_PCK:
	case INSPECT_MODE_REFRAME:
		gf_filter_pid_set_framing_mode(pid, GF_TRUE);
		break;
	default:
		gf_filter_pid_set_framing_mode(pid, GF_FALSE);
		break;
	}

	if (!ctx->is_prober) {
		pctx->dump_pid = 1;
	}

	gf_filter_pid_init_play_event(pid, &evt, ctx->start, ctx->speed, "Inspect");
	gf_filter_pid_send_event(pid, &evt);

	if (ctx->is_prober || ctx->deep || ctx->fmt) {
		ctx->dump_pck = GF_TRUE;
	} else {
		ctx->dump_pck = GF_FALSE;
	}
	if (ctx->pcr)
		gf_filter_pid_set_clock_mode(pid, GF_TRUE);
	return GF_OK;
}

static const GF_FilterCapability InspecterDemuxedCaps[] =
{
	//accept any stream but files, framed
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_CODECID, GF_CODECID_NONE),
	{0},
};

static const GF_FilterCapability InspecterReframeCaps[] =
{
	//accept any stream but files, framed
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_CODECID, GF_CODECID_NONE),
	CAP_BOOL(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_UNFRAMED, GF_TRUE),
	{0},
};

GF_Err inspect_initialize(GF_Filter *filter)
{
	const char *name = gf_filter_get_name(filter);
	GF_InspectCtx  *ctx = (GF_InspectCtx *) gf_filter_get_udta(filter);

	if (name && !strcmp(name, "probe") ) {
		ctx->is_prober = GF_TRUE;
		return GF_OK;
	}

	if (!ctx->log) return GF_BAD_PARAM;
	if (!strcmp(ctx->log, "stderr")) ctx->dump = stderr;
	else if (!strcmp(ctx->log, "stdout")) ctx->dump = stdout;
	else {
		ctx->dump = gf_fopen(ctx->log, "wt");
		if (!ctx->dump) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("[Inspec] Failed to open file %s\n", ctx->log));
			return GF_IO_ERR;
		}
	}
	if (ctx->analyze) {
		ctx->xml = GF_TRUE;
	}

	if (ctx->xml) {
		ctx->fmt = NULL;
		gf_fprintf(ctx->dump, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
		gf_fprintf(ctx->dump, "<GPACInspect>\n");
	}

	switch (ctx->mode) {
	case INSPECT_MODE_RAW:
		break;
	case INSPECT_MODE_REFRAME:
		gf_filter_override_caps(filter, InspecterReframeCaps,  sizeof(InspecterReframeCaps)/sizeof(GF_FilterCapability) );
		break;
	default:
		gf_filter_override_caps(filter, InspecterDemuxedCaps,  sizeof(InspecterDemuxedCaps)/sizeof(GF_FilterCapability) );
		break;
	}
	return GF_OK;
}

static Bool inspect_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_InspectCtx  *ctx = (GF_InspectCtx *) gf_filter_get_udta(filter);
	if (!ctx->info) return GF_TRUE;
	if (evt->base.type!=GF_FEVT_INFO_UPDATE) return GF_TRUE;
	PidCtx *pctx = gf_filter_pid_get_udta(evt->base.on_pid);
	pctx->dump_pid = 2;
	return GF_TRUE;
}


#define OFFS(_n)	#_n, offsetof(GF_InspectCtx, _n)
static const GF_FilterArgs InspectArgs[] =
{
	{ OFFS(log), "set inspect log filename", GF_PROP_STRING, "stderr", "fileName, stderr or stdout", 0},
	{ OFFS(mode), "dump mode\n"
	"- pck: dump full packet\n"
	"- blk: dump packets before reconstruction\n"
	"- frame: force reframer\n"
	"- raw: dump source packets without demuxing", GF_PROP_UINT, "pck", "pck|blk|frame|raw", 0},
	{ OFFS(interleave), "dump packets as they are received on each pid. If false, report per pid is generated", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(deep), "dump packets along with PID state change, implied when [-fmt]() is set", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(props), "dump packet properties, ignored when [-fmt]() is set (see filter help)", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(dump_data), "enable full data dump (__WARNING__ heavy!), ignored when [-fmt]() is set (see filter help)", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(fmt), "set packet dump format (see filter help)", GF_PROP_STRING, NULL, NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(hdr), "print a header corresponding to fmt string without \'$ \'or \"pid.\"", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(allp), "analyse for the entire duration, rather than stoping when all pids are found", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(info), "monitor PID info changes", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(pcr), "dump M2TS PCR info", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_EXPERT},
	{ OFFS(speed), "set playback command speed. If speed is negative and start is 0, start is set to -1", GF_PROP_DOUBLE, "1.0", NULL, 0},
	{ OFFS(start), "set playback start offset. Negative value means percent of media dur with -1 <=> dur", GF_PROP_DOUBLE, "0.0", NULL, 0},
	{ OFFS(dur), "set inspect duration", GF_PROP_FRACTION, "0/0", NULL, 0},
	{ OFFS(analyze), "analyze sample content (NALU, OBU)", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(xml), "use xml formatting (implied if (-analyze]() is set) and disable [-fmt]()", GF_PROP_BOOL, "false", NULL, 0},
	{ OFFS(fftmcd), "consider timecodes use ffmpeg-compatible signaling rather than QT compliant one", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_EXPERT},
	{ OFFS(dtype), "dump property type", GF_PROP_BOOL, "false", NULL, 0},
	{ OFFS(test), "skip predefined set of properties, used for test mode\n"
		"- no: no properties skipped\n"
		"- noprop: all properties/info changes on pid are skipped, only packets are dumped\n"
		"- network: URL/path dump, cache state, file size properties skipped (used for hashing network results)\n"
		"- encode: same as network plus skip decoder config (used for hashing encoding results)", GF_PROP_UINT, "no", "no|noprop|network|encode", GF_FS_ARG_HINT_EXPERT},
	{0}
};

static const GF_FilterCapability InspectCaps[] =
{
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_STREAM_TYPE, GF_STREAM_UNKNOWN),
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_CODECID, GF_CODECID_NONE),
};

const GF_FilterRegister InspectRegister = {
	.name = "inspect",
	GF_FS_SET_DESCRIPTION("Inspect packets")
	GF_FS_SET_HELP("The inspect filter can be used to dump pid and packets. It may also be used to check parts of payload of the packets. The default options inspect only pid changes.\n"\
				"The packet inspector can be configured to dump specific properties of packets using [-fmt]().\n"\
	 			"When the option is not present, all properties are dumped. Otherwise, only properties identified by `$TOKEN$` "
	 			"are printed. You may use '$', '@' or '%' for `TOKEN` separator. `TOKEN` can be:\n"\
				"- pn: packet (frame in framed mode) number\n"\
				"- dts: decoding time stamp in stream timescale, N/A if not available\n"\
				"- ddts: difference between current and previous packets decoding time stamp in stream timescale, N/A if not available\n"\
				"- cts: composition time stamp in stream timescale, N/A if not available\n"\
				"- dcts: difference between current and previous packets composition time stamp in stream timescale, N/A if not available\n"\
				"- ctso: difference between composition time stamp and decoding time stamp in stream timescale, N/A if not available\n"\
				"- dur: duration in stream timescale\n"\
				"- frame: framing status\n"
				"  - interface: complete AU, interface object (no size info). Typically a GL texture\n"
				"  - frame_full: complete AU\n"
				"  - frame_start: begining of frame\n"
				"  - frame_end: end of frame\n"
				"  - frame_cont: frame continuation (not begining, not end)\n"
				"- sap or rap: SAP type of the frame\n"\
				"- ilace: interlacing flag (0: progressive, 1: top field, 2: bottom field)\n"\
				"- corr: corrupted packet flag\n"\
				"- seek: seek flag\n"\
				"- bo: byte offset in source, N/A if not available\n"\
				"- roll: roll info\n"\
				"- crypt: crypt flag\n"\
				"- vers: carrousel version number\n"\
				"- size: size of packet\n"\
				"- crc: 32 bit CRC of packet\n"\
				"- lf: insert linefeed\n"\
				"- cr: insert carriage return\n"\
				"- data: hex dump of packet (** WARNING, BIG OUTPUT !! **)\n"\
				"- lp: leading picture flag\n"\
				"- depo: depends on other packet flag\n"\
				"- depf: is depended on other packet flag\n"\
				"- red: redundant coding flag\n"\
				"- ck: clock type used for PCR discontinuities\n"\
	 			"- P4CC: 4CC of packet property\n"\
	 			"- PropName: Name of packet property\n"\
	 			"- pid.P4CC: 4CC of PID property\n"\
	 			"- pid.PropName: Name of PID property\n"\
	 			"\n"\
	 			"EX fmt=\"PID $pid.ID$ packet $pn$ DTS $dts$ CTS $cts$ $lf$\"\n"
	 			"This dumps packet number, cts and dts as follows: `PID 1 packet 10 DTS 100 CTS 108 \\n`\n"\
	 			"  \n"\
	 			"An unrecognized keywork or missing property will resolve to an empty string.\n"\
	 			"\n"\
	 			"Note: when dumping in interleaved mode, there is no guarantee that the packets will be dumped in their original sequence order since the inspector fetches one packet at a time on each PID.\n")
	.private_size = sizeof(GF_InspectCtx),
	.flags = GF_FS_REG_EXPLICIT_ONLY,
	.max_extra_pids = (u32) -1,
	.args = InspectArgs,
	SETCAPS(InspectCaps),
	.initialize = inspect_initialize,
	.finalize = inspect_finalize,
	.process = inspect_process,
	.process_event = inspect_process_event,
	.configure_pid = inspect_config_input,
};

static const GF_FilterCapability ProberCaps[] =
{
	//accept any stream but files, framed
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_SCENE),
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_OD),
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_TEXT),
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_CODECID, GF_CODECID_NONE),
	CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
	{0},
	CAP_UINT(GF_CAPS_INPUT,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_SCENE),
	CAP_UINT(GF_CAPS_INPUT,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_OD),
	CAP_UINT(GF_CAPS_INPUT,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_TEXT),
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_CODECID, GF_CODECID_NONE),
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_CODECID, GF_CODECID_RAW),
	CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
	{0},
};


const GF_FilterRegister ProbeRegister = {
	.name = "probe",
	GF_FS_SET_DESCRIPTION("Probe source")
	GF_FS_SET_HELP("The Probe filter is used by applications (typically `MP4Box`) to query demuxed pids available in a source chain.\n"
	"The filter does not produce any output nor feedback, it is up to the app developper to query input pids of the prober and take appropriated decisions.")
	.private_size = sizeof(GF_InspectCtx),
	.flags = GF_FS_REG_EXPLICIT_ONLY,
	.max_extra_pids = (u32) -1,
	.args = InspectArgs,
	.initialize = inspect_initialize,
	SETCAPS(ProberCaps),
	.finalize = inspect_finalize,
	.process = inspect_process,
	.configure_pid = inspect_config_input,
};

const GF_FilterRegister *inspect_register(GF_FilterSession *session)
{
	return &InspectRegister;
}

const GF_FilterRegister *probe_register(GF_FilterSession *session)
{
	return &ProbeRegister;
}




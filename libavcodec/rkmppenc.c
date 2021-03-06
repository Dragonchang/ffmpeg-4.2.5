/*
 * RockChip MPP Video Encoder
 * Copyright (c) 2018 hertz.wang@rock-chips.com
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <drm_fourcc.h>
#include <pthread.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_mpi.h>
#include <time.h>
#include <unistd.h>

#include "avcodec.h"
#include "hwaccel.h"
#include "internal.h"
#include "libavutil/avassert.h"
#include "libavutil/buffer.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/log.h"

// copy from mpp/base/inc/mpp_packet_impl.h
#define MPP_PACKET_FLAG_INTRA       (0x00000008)

#define SEND_FRAME_TIMEOUT          100
#define RECEIVE_PACKET_TIMEOUT      100

typedef struct {
    MppCtx ctx;
    MppApi *mpi;

    char eos_reached;
} RKMPPEncoder;

typedef struct {
    AVClass *av_class;
    AVBufferRef *encoder_ref;
} RKMPPEncodeContext;

typedef struct {
    MppPacket packet;
    AVBufferRef *encoder_ref;
} RKMPPPacketContext;

static MppCodingType rkmpp_get_codingtype(AVCodecContext *avctx)
{
    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:          return MPP_VIDEO_CodingAVC;
    default:                        return MPP_VIDEO_CodingUnused;
    }
}

static MppFrameFormat rkmpp_get_mppformat(enum AVPixelFormat avformat)
{
    switch (avformat) {
    case AV_PIX_FMT_NV12:           return MPP_FMT_YUV420SP;
    case AV_PIX_FMT_YUV420P:        return MPP_FMT_YUV420P;
    case AV_PIX_FMT_YUYV422:        return MPP_FMT_YUV422_YUYV;
    case AV_PIX_FMT_UYVY422:        return MPP_FMT_YUV422_UYVY;
#ifdef DRM_FORMAT_NV12_10
    case AV_PIX_FMT_P010:           return MPP_FMT_YUV420SP_10BIT;
#endif
    default:                        return -1;
    }
}

static int rkmpp_close_encoder(AVCodecContext *avctx)
{
    RKMPPEncodeContext *rk_context = avctx->priv_data;
    av_buffer_unref(&rk_context->encoder_ref);
    return 0;
}

static void rkmpp_release_encoder(void *opaque, uint8_t *data)
{
    RKMPPEncoder *encoder = (RKMPPEncoder *)data;

    if (encoder->mpi) {
        encoder->mpi->reset(encoder->ctx);
        mpp_destroy(encoder->ctx);
        encoder->ctx = NULL;
    }

    av_free(encoder);
}

static int rkmpp_preg_config(AVCodecContext *avctx, RKMPPEncoder *encoder,
                             MppEncPrepCfg *prep_cfg)
{
    int ret;

    memset(prep_cfg, 0, sizeof(*prep_cfg));
    prep_cfg->change        = MPP_ENC_PREP_CFG_CHANGE_INPUT |
                              MPP_ENC_PREP_CFG_CHANGE_ROTATION |
                              MPP_ENC_PREP_CFG_CHANGE_FORMAT;
    prep_cfg->width         = avctx->width;
    prep_cfg->height        = avctx->height;
    prep_cfg->hor_stride    = avctx->width;
    prep_cfg->ver_stride    = avctx->height;
    prep_cfg->format        = rkmpp_get_mppformat(avctx->sw_pix_fmt);
    prep_cfg->rotation      = MPP_ENC_ROT_0;

    ret = encoder->mpi->control(encoder->ctx, MPP_ENC_SET_PREP_CFG, prep_cfg);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set prep cfg on MPI (code = %d).\n", ret);
        return AVERROR_UNKNOWN;
    }

    return 0;
}

static int rkmpp_rc_config(AVCodecContext *avctx, RKMPPEncoder *encoder,
                           MppEncRcCfg *rc_cfg)
{
    int ret;
    int fps;

    memset(rc_cfg, 0, sizeof(*rc_cfg));
    rc_cfg->change  = MPP_ENC_RC_CFG_CHANGE_ALL;
    // TODO: set by AVOption
    rc_cfg->rc_mode = MPP_ENC_RC_MODE_CBR;
    rc_cfg->quality = MPP_ENC_RC_QUALITY_MEDIUM;

    if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_CBR) {
        /* constant bitrate has very small bps range of 1/16 bps */
        rc_cfg->bps_target  = avctx->bit_rate;
        rc_cfg->bps_max     = avctx->bit_rate * 17 / 16;
        rc_cfg->bps_min     = avctx->bit_rate * 15 / 16;
    } else if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_VBR) {
        if (rc_cfg->quality == MPP_ENC_RC_QUALITY_CQP) {
            /* constant QP does not have bps */
            rc_cfg->bps_target  = -1;
            rc_cfg->bps_max     = -1;
            rc_cfg->bps_min     = -1;
        } else {
            /* variable bitrate has large bps range */
            rc_cfg->bps_target  = avctx->bit_rate;
            rc_cfg->bps_max     = avctx->bit_rate * 17 / 16;
            rc_cfg->bps_min     = avctx->bit_rate * 1 / 16;
        }
    }

    fps = avctx->time_base.den / avctx->time_base.num;
    /* fix input / output frame rate */
    rc_cfg->fps_in_flex     = 0;
    rc_cfg->fps_in_num      = fps;
    rc_cfg->fps_in_denorm   = 1;
    rc_cfg->fps_out_flex    = 0;
    rc_cfg->fps_out_num     = fps;
    rc_cfg->fps_out_denorm  = 1;

    rc_cfg->gop             = avctx->gop_size;
    rc_cfg->skip_cnt        = 0;

    ret = encoder->mpi->control(encoder->ctx, MPP_ENC_SET_RC_CFG, rc_cfg);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set rc cfg on MPI (code = %d).\n", ret);
        return AVERROR_UNKNOWN;
    }

    return 0;
}

static int rkmpp_codec_config(AVCodecContext *avctx, RKMPPEncoder *encoder,
                              MppCodingType codectype, MppEncRcCfg *rc_cfg,
                              MppEncCodecCfg *codec_cfg)
{
    int ret;

    memset(codec_cfg, 0, sizeof(*codec_cfg));
    codec_cfg->coding = codectype;
    switch (codectype) {
    case MPP_VIDEO_CodingAVC: {
        int qp_min = avctx->qmin,
            qp_max = avctx->qmax,
            qp_step = avctx->max_qdiff;
        int qp_init = 26;
        codec_cfg->h264.change = MPP_ENC_H264_CFG_CHANGE_PROFILE |
                                 MPP_ENC_H264_CFG_CHANGE_ENTROPY |
                                 MPP_ENC_H264_CFG_CHANGE_TRANS_8x8 |
                                 MPP_ENC_H264_CFG_CHANGE_QP_LIMIT;
        /*
         * H.264 profile_idc parameter
         * Support: Baseline profile
         *          Main profile
         *          High profile
         */
        if (avctx->profile != FF_PROFILE_H264_BASELINE &&
            avctx->profile != FF_PROFILE_H264_MAIN &&
            avctx->profile != FF_PROFILE_H264_HIGH) {
            av_log(avctx, AV_LOG_INFO, "Unsupport profile %d, force set to %d\n",
                   avctx->profile, FF_PROFILE_H264_HIGH);
            avctx->profile = FF_PROFILE_H264_HIGH;
        }
        codec_cfg->h264.profile = avctx->profile;

        /*
         * H.264 level_idc parameter
         * 10 / 11 / 12 / 13    - qcif@15fps / cif@7.5fps / cif@15fps / cif@30fps
         * 20 / 21 / 22         - cif@30fps / half-D1@@25fps / D1@12.5fps
         * 30 / 31 / 32         - D1@25fps / 720p@30fps / 720p@60fps
         * 40 / 41 / 42         - 1080p@30fps / 1080p@30fps / 1080p@60fps
         * 50 / 51 / 52         - 4K@30fps
         */
        if (avctx->level == FF_LEVEL_UNKNOWN) {
            av_log(avctx, AV_LOG_INFO, "Unsupport level %d, force set to %d\n",
                   avctx->level, 51);
            avctx->level = 51;
        }
        codec_cfg->h264.level               = avctx->level;
        codec_cfg->h264.entropy_coding_mode =
            (codec_cfg->h264.profile == FF_PROFILE_H264_HIGH) ? 1 : 0;
        codec_cfg->h264.cabac_init_idc      = 0;
        codec_cfg->h264.transform8x8_mode   = 1;

        if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_CBR) {
            /* constant bitrate do not limit qp range */
            qp_max  = 48;
            qp_min  = 4;
            qp_step = 16;
            qp_init = 0;
        } else if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_VBR) {
            if (rc_cfg->quality == MPP_ENC_RC_QUALITY_CQP) {
                /* constant QP mode qp is fixed */
                qp_max   = qp_init;
                qp_min   = qp_init;
                qp_step  = 0;
            } else {
                /* variable bitrate has qp min limit */
                qp_max   = 40;
                qp_min   = 12;
                qp_step  = 8;
                qp_init  = 0;
            }
        }

        codec_cfg->h264.qp_max      = qp_max;
        codec_cfg->h264.qp_min      = qp_min;
        codec_cfg->h264.qp_max_step = qp_step;
        codec_cfg->h264.qp_init     = qp_init;
    } break;
    case MPP_VIDEO_CodingMJPEG:
        codec_cfg->jpeg.change  = MPP_ENC_JPEG_CFG_CHANGE_QP;
        codec_cfg->jpeg.quant   = 10; // 1 ~ 10
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "TODO encoder coding type %d\n", codectype);
        return AVERROR_UNKNOWN;
    }

    ret = encoder->mpi->control(encoder->ctx, MPP_ENC_SET_CODEC_CFG, codec_cfg);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set codec cfg on MPI (code = %d).\n", ret);
        return AVERROR_UNKNOWN;
    }

    return 0;
}

static int rkmpp_init_encoder(AVCodecContext *avctx)
{
    int ret;
    MppCodingType codectype;
    RKMPPEncodeContext *rk_context;
    RKMPPEncoder *encoder;
    MppEncPrepCfg prep_cfg;
    MppEncRcCfg rc_cfg;
    MppEncCodecCfg codec_cfg;
    RK_S64 paramS64;
    MppEncSeiMode sei_mode;
    MppPacket packet = NULL;

    rk_context = avctx->priv_data;
    rk_context->encoder_ref = NULL;
    codectype = rkmpp_get_codingtype(avctx);
    if (codectype == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unsupport codec type (%d).\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_check_support_format(MPP_CTX_ENC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Codec type (%d) unsupported by MPP\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // create a encoder and a ref to it
    encoder = av_mallocz(sizeof(RKMPPEncoder));
    if (!encoder) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    rk_context->encoder_ref =
        av_buffer_create((uint8_t *)encoder, sizeof(*encoder),
                         rkmpp_release_encoder, NULL, AV_BUFFER_FLAG_READONLY);
    if (!rk_context->encoder_ref) {
        av_free(encoder);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "Initializing RKMPP encoder.\n");

    // mpp init
    ret = mpp_create(&encoder->ctx, &encoder->mpi);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_init(encoder->ctx, MPP_CTX_ENC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // mpp setup
    ret = rkmpp_preg_config(avctx, encoder, &prep_cfg);
    if (ret)
        goto fail;

    ret = rkmpp_rc_config(avctx, encoder, &rc_cfg);
    if (ret)
        goto fail;

    ret = rkmpp_codec_config(avctx, encoder, codectype, &rc_cfg, &codec_cfg);
    if (ret)
        goto fail;

    sei_mode = MPP_ENC_SEI_MODE_ONE_FRAME;
    ret = encoder->mpi->control(encoder->ctx, MPP_ENC_SET_SEI_CFG, &sei_mode);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set sei cfg on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // TODO: osd if hardware support

    paramS64 = SEND_FRAME_TIMEOUT;
    ret = encoder->mpi->control(encoder->ctx, MPP_SET_INPUT_TIMEOUT, &paramS64);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set input timeout on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    paramS64 = RECEIVE_PACKET_TIMEOUT;
    ret = encoder->mpi->control(encoder->ctx, MPP_SET_OUTPUT_TIMEOUT, &paramS64);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set output timeout on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = encoder->mpi->control(encoder->ctx, MPP_ENC_GET_EXTRA_INFO, &packet);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get extra info on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    if (packet) {
        /* get and write sps/pps for H.264 */
        void *ptr   = mpp_packet_get_pos(packet);
        size_t len  = mpp_packet_get_length(packet);

        if (avctx->extradata != NULL && avctx->extradata_size != len) {
            av_free(avctx->extradata);
            avctx->extradata = NULL;
        }
        if (!avctx->extradata)
            avctx->extradata = av_malloc(len);
        if (avctx->extradata == NULL) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        avctx->extradata_size = len;
        memcpy(avctx->extradata, ptr, len);

        packet = NULL;
    }

    av_log(avctx, AV_LOG_DEBUG, "RKMPP encoder initialized successfully.\n");

    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP encoder.\n");
    rkmpp_close_encoder(avctx);
    return ret;
}

static int rkmpp_queue_frame(AVCodecContext *avctx, RKMPPEncoder *encoder,
                             const AVFrame *avframe, MppFrame *out_frame)
{
    int ret;
    MppFrameFormat mppformat;
    MppCtx ctx;
    MppApi *mpi;
    MppBuffer buffer = NULL;
    MppBufferInfo info;
    MppFrame frame = NULL;
    MppTask task = NULL;

    // check format
    if (avframe) {
        enum AVPixelFormat swformat;
        if(avframe->format != AV_PIX_FMT_DRM_PRIME) {
            av_log(avctx, AV_LOG_ERROR, "RKMPPEncoder only support fmt DRM\n");
            return AVERROR(EINVAL);
        }
        swformat    = ((AVHWFramesContext*)avframe->hw_frames_ctx->data)->sw_format;
        mppformat   = rkmpp_get_mppformat(swformat);
        if (mppformat < 0) {
            av_log(avctx, AV_LOG_ERROR, "Unsupport av format %d\n", swformat);
            return AVERROR(EINVAL);
        }
    }

    ret = mpp_frame_init(&frame);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed init mpp frame on encoder (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }
    mpp_frame_set_eos(frame, encoder->eos_reached);

    if (avframe) {
        AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor*)avframe->data[0];
        AVDRMLayerDescriptor *layer = &desc->layers[0];

        mpp_frame_set_pts(frame, avframe->pts);
        mpp_frame_set_dts(frame, avframe->pkt_dts);
        mpp_frame_set_width(frame, avframe->width);
        mpp_frame_set_height(frame, avframe->height);
        if (mppformat == MPP_FMT_YUV422_YUYV || mppformat == MPP_FMT_YUV422_UYVY) {
            mpp_frame_set_hor_stride(frame, 2 * layer->planes[0].pitch/* /bpp */);
        } else {
            // nv12 or yuv420p
            mpp_frame_set_hor_stride(frame, layer->planes[0].pitch/* /bpp */);
        }
        if (layer->nb_planes > 1)
            mpp_frame_set_ver_stride(frame,
                layer->planes[1].offset / layer->planes[0].pitch);
        else
            mpp_frame_set_ver_stride(frame, avframe->height);
        mpp_frame_set_fmt(frame, mppformat);

        memset(&info, 0, sizeof(info));
        info.type   = MPP_BUFFER_TYPE_ION;
        info.size   = desc->objects[0].size;
        info.fd     = desc->objects[0].fd;
        //info.ptr    = desc->objects[0].ptr;
        ret = mpp_buffer_import(&buffer, &info);
        if (ret != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to import buffer\n");
            ret = AVERROR(EINVAL);
            goto out;
        }
        mpp_frame_set_buffer(frame, buffer);
    }

    ctx = encoder->ctx;
    mpi = encoder->mpi;
    ret = mpi->poll(ctx, MPP_PORT_INPUT, MPP_POLL_BLOCK);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to poll task input (ret = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto out;
    }

    ret = mpi->dequeue(ctx, MPP_PORT_INPUT, &task);
    if (ret != MPP_OK || NULL == task) {
        av_log(avctx, AV_LOG_ERROR, "Failed to dequeue task input (ret = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto out;
    }

    mpp_task_meta_set_frame (task, KEY_INPUT_FRAME, frame);
    ret = mpi->enqueue(ctx, MPP_PORT_INPUT, task);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to enqueue task input (ret = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto out;
    }
    *out_frame = frame;
    frame = NULL;

out:
    if (buffer)
        mpp_buffer_put(buffer);
    if (frame)
        mpp_frame_deinit(&frame);
    return 0;
}

static int rkmpp_send_frame(AVCodecContext *avctx, const AVFrame *frame,
                            MppFrame *mpp_frame)
{
    int ret;
    RKMPPEncodeContext *rk_context = avctx->priv_data;
    RKMPPEncoder *encoder = (RKMPPEncoder *)rk_context->encoder_ref->data;

    if (!frame) {
        av_log(avctx, AV_LOG_DEBUG, "End of stream.\n");
        encoder->eos_reached = 1;
        ret = rkmpp_queue_frame(avctx, encoder, NULL, mpp_frame);
        if (ret)
            av_log(avctx, AV_LOG_ERROR, "Failed to send EOS to encoder (code = %d)\n", ret);
        return ret;
    }

    ret = rkmpp_queue_frame(avctx, encoder, frame, mpp_frame);
    if (ret && ret != AVERROR(EAGAIN))
        av_log(avctx, AV_LOG_ERROR, "Failed to send frame to encoder (code = %d)\n", ret);

    return ret;
}

static void rkmpp_release_packet(void *opaque, uint8_t *data)
{
    RKMPPPacketContext *pkt_ctx = (RKMPPPacketContext *)opaque;

    mpp_packet_deinit(&pkt_ctx->packet);
    av_buffer_unref(&pkt_ctx->encoder_ref);
    av_free(pkt_ctx);
}

static int rkmpp_receive_packet(AVCodecContext *avctx, AVPacket *pkt,
                                MppFrame *mpp_frame)
{
    int ret;
    RKMPPEncodeContext *rk_context = avctx->priv_data;
    RKMPPEncoder *encoder = (RKMPPEncoder *)rk_context->encoder_ref->data;
    MppCtx ctx = encoder->ctx;
    MppApi *mpi = encoder->mpi;
    MppTask task = NULL;
    MppPacket packet = NULL;

    ret = mpi->poll(ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to poll task output (ret = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpi->dequeue(ctx, MPP_PORT_OUTPUT, &task);
    if (ret || NULL == task) {
        av_log(avctx, AV_LOG_ERROR, "Failed to dequeue task output (ret = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    if (task) {
        mpp_task_meta_get_packet(task, KEY_OUTPUT_PACKET, &packet);
        ret = mpi->enqueue(ctx, MPP_PORT_OUTPUT, task);
        if (ret != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to enqueue task output (ret = %d)\n", ret);
            ret = AVERROR_UNKNOWN;
            goto fail;
        }
    }

    if (packet) {
        RKMPPPacketContext *pkt_ctx;
        RK_U32 flag;

        if (mpp_packet_get_eos(packet)) {
            av_log(avctx, AV_LOG_DEBUG, "Received a EOS packet.\n");
            if (encoder->eos_reached) {
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        pkt_ctx = av_mallocz(sizeof(*pkt_ctx));
        if (!pkt_ctx) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        pkt_ctx->packet = packet;
        pkt_ctx->encoder_ref = av_buffer_ref(rk_context->encoder_ref);

        // TODO: outside need fd from mppbuffer?
        pkt->data = mpp_packet_get_data(packet);
        pkt->size = mpp_packet_get_length(packet);
        pkt->buf = av_buffer_create((uint8_t*)pkt->data, pkt->size,
            rkmpp_release_packet, pkt_ctx, AV_BUFFER_FLAG_READONLY);
        if (!pkt->buf) {
            av_buffer_unref(&pkt_ctx->encoder_ref);
            av_free(pkt_ctx);
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        pkt->pts = mpp_packet_get_pts(packet);
        pkt->dts = mpp_packet_get_dts(packet);
        if (pkt->pts <= 0)
            pkt->pts = pkt->dts;
        if (pkt->dts <= 0)
            pkt->dts = pkt->pts;
        flag = mpp_packet_get_flag(packet);
        if (flag & MPP_PACKET_FLAG_INTRA)
            pkt->flags |= AV_PKT_FLAG_KEY;

        packet = NULL;
    }

fail:
    if (packet)
        mpp_packet_deinit(&packet);
    if (*mpp_frame) {
        mpp_frame_deinit(mpp_frame);
        *mpp_frame = NULL;
    }
    return ret;
}

static int rkmpp_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                              const AVFrame *frame, int *got_packet)
{
    int ret;
    MppFrame mpp_frame = NULL;

    ret = rkmpp_send_frame(avctx, frame, &mpp_frame);
    if (ret)
        return ret;

    ret = rkmpp_receive_packet(avctx, pkt, &mpp_frame);
    av_assert0(mpp_frame == NULL);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        *got_packet = 0;
    } else if (ret) {
        return ret;
    } else {
        *got_packet = 1;
    }

    return 0;
}

static const AVCodecHWConfigInternal *rkmpp_hw_configs[] = {
    HW_CONFIG_INTERNAL(DRM_PRIME),
    NULL
};

#define RKMPP_ENC_CLASS(NAME) \
    static const AVClass rkmpp_##NAME##_enc_class = { \
        .class_name = "rkmpp_" #NAME "_enc", \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

// TODO: .send_frame .receive_packet
#define RKMPP_ENC(NAME, ID, BSFS) \
    RKMPP_ENC_CLASS(NAME) \
    AVCodec ff_##NAME##_rkmpp_encoder = { \
        .name           = #NAME "_rkmpp", \
        .long_name      = NULL_IF_CONFIG_SMALL(#NAME " (rkmpp)"), \
        .type           = AVMEDIA_TYPE_VIDEO, \
        .id             = ID, \
        .init           = rkmpp_init_encoder, \
        .close          = rkmpp_close_encoder, \
        .encode2        = rkmpp_encode_frame, \
        .priv_data_size = sizeof(RKMPPEncodeContext), \
        .priv_class     = &rkmpp_##NAME##_enc_class, \
        .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE, \
        .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP, \
        .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_DRM_PRIME, \
                                                         AV_PIX_FMT_NONE }, \
        .hw_configs     = rkmpp_hw_configs, \
        .bsfs           = BSFS, \
        .wrapper_name   = "rkmpp", \
    };

RKMPP_ENC(h264, AV_CODEC_ID_H264, NULL)

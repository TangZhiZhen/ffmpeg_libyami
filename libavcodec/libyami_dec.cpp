/*
 * Intel Yet Another Media Infrastructure video decoder/encoder
 *
 * Copyright (c) 2016 Intel Corporation
 *     Zhou Yun(yunx.z.zhou@intel.com)
 *     Jun Zhao(jun.zhao@intel.com)
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
#include <pthread.h>
#include <unistd.h>
#include <deque>

extern "C" {
#include "avcodec.h"
#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "internal.h"
#include "libavutil/internal.h"
}
#include "VideoDecoderHost.h"
#include "libyami.h"
#include "libyami_dec.h"

using namespace YamiMediaCodec;

static int ff_yami_decode_thread_init(YamiDecContext *s)
{
    int ret = 0;
    if (!s)
        return -1;
    if ((ret = pthread_mutex_init(&s->ctx_mutex, NULL)) < 0)
        return ret;
    if ((ret = pthread_mutex_init(&s->in_mutex, NULL)) < 0)
        return ret;
    if ((ret = pthread_cond_init(&s->in_cond, NULL)) < 0)
        return ret;
    s->decode_status = DECODE_THREAD_NOT_INIT;
    return 0;
}

static int ff_yami_decode_thread_close(YamiDecContext *s)
{
    if (!s)
        return -1;
    pthread_mutex_lock(&s->ctx_mutex);
    /* if decode thread do not create do not loop */
    while (s->decode_status != DECODE_THREAD_EXIT
           && s->decode_status != DECODE_THREAD_NOT_INIT) {
        s->decode_status = DECODE_THREAD_GOT_EOS;
        pthread_mutex_unlock(&s->ctx_mutex);
        pthread_cond_signal(&s->in_cond);
        pthread_mutex_lock(&s->ctx_mutex);
        av_usleep(10000);
    }
    pthread_mutex_unlock(&s->ctx_mutex);
    pthread_mutex_destroy(&s->in_mutex);
    pthread_cond_destroy(&s->in_cond);
    return 0;
}

static void *ff_yami_decode_thread(void *arg)
{
    AVCodecContext *avctx = (AVCodecContext *)arg;
    YamiDecContext *s = (YamiDecContext *)avctx->priv_data;
    while (1) {
        VideoDecodeBuffer *in_buffer = NULL;

        av_log(avctx, AV_LOG_VERBOSE, "decode thread running ...\n");
        /* when in queue is empty and don't get EOS, waiting, else
           flush the decode buffer with NULL */
        pthread_mutex_lock(&s->in_mutex);
        if (s->in_queue->empty()) {
            if (s->decode_status == DECODE_THREAD_GOT_EOS) {
                /* flush the decode buffer with NULL when get EOS */
                VideoDecodeBuffer flush_buffer;
                flush_buffer.data = NULL;
                flush_buffer.size = 0;
                s->decoder->decode(&flush_buffer);
                pthread_mutex_unlock(&s->in_mutex);
                break;
            }

            av_log(avctx, AV_LOG_VERBOSE, "decode thread waiting with empty queue.\n");
            pthread_cond_wait(&s->in_cond, &s->in_mutex); /* wait the packet to decode */
            pthread_mutex_unlock(&s->in_mutex);
            continue;
        }

        av_log(avctx, AV_LOG_VERBOSE, "in queue size %ld\n", s->in_queue->size());
        /* get a packet from in queue and decode */
        in_buffer = s->in_queue->front();
        pthread_mutex_unlock(&s->in_mutex);
        av_log(avctx, AV_LOG_VERBOSE, "process input buffer, [data=%p, size=%zu]\n",
               in_buffer->data, in_buffer->size);
        Decode_Status status = s->decoder->decode(in_buffer);
        av_log(avctx, AV_LOG_VERBOSE, "decode status %d, decoded count %d render count %d\n",
               status, s->decode_count_yami, s->render_count);
        /* get the format info when the first decode success */
        if (DECODE_SUCCESS == status && !s->format_info) {
            s->format_info = s->decoder->getFormatInfo();
            av_log(avctx, AV_LOG_VERBOSE, "decode format %dx%d\n",
                   s->format_info->width,s->format_info->height);
            if (s->format_info) {
                avctx->width  = s->format_info->width;
                avctx->height = s->format_info->height;
            }
        }

        /* when format change, update format info and re-send the
           packet to decoder */
        if (DECODE_FORMAT_CHANGE == status) {
            s->format_info = s->decoder->getFormatInfo();
            if (s->format_info) {
                avctx->width  = s->format_info->width;
                avctx->height = s->format_info->height;
                av_log(avctx, AV_LOG_VERBOSE, "decode format change %dx%d\n",
                   s->format_info->width,s->format_info->height);
            }
            status = s->decoder->decode(in_buffer);
            if (status < 0) {
                av_log(avctx, AV_LOG_ERROR, "decode error %d\n", status);
            }
        }

        if (status < 0 || !s->format_info) {
            av_log(avctx, AV_LOG_ERROR, "decode error %d\n", status);
            break;
        }

        s->decode_count_yami++;

        pthread_mutex_lock(&s->in_mutex);
        s->in_queue->pop_front();
        pthread_mutex_unlock(&s->in_mutex);
        av_free(in_buffer->data);
        av_free(in_buffer);
    }

    av_log(avctx, AV_LOG_VERBOSE, "decode thread exit\n");
    pthread_mutex_lock(&s->ctx_mutex);
    s->decode_status = DECODE_THREAD_EXIT;
    pthread_mutex_unlock(&s->ctx_mutex);
    return NULL;
}

static void ff_yami_recycle_frame(void *opaque, uint8_t *data)
{
    AVCodecContext *avctx = (AVCodecContext *)opaque;
    YamiDecContext *s = (YamiDecContext *)avctx->priv_data;
    YamiImage *yami_image = (YamiImage *)data;
    if (!s || !s->decoder || !yami_image)
        return;
    pthread_mutex_lock(&s->ctx_mutex);
    /* XXX: should I delete frame buffer?? */
    yami_image->output_frame.reset();
    av_free(yami_image);
    pthread_mutex_unlock(&s->ctx_mutex);
    av_log(avctx, AV_LOG_DEBUG, "recycle previous frame: %p\n", yami_image);
}

/*
 * when decode output format is YAMI, don't move the decoded data from GPU to CPU,
 * otherwise, used the USWC memory copy. maybe change this solution with generic
 * hardware surface upload/download filter "hwupload/hwdownload"
 */
static int ff_convert_to_frame(AVCodecContext *avctx, YamiImage *from, AVFrame *to)
{
    if(!avctx || !from || !to)
        return -1;
    if (avctx->pix_fmt == AV_PIX_FMT_YAMI) {
        to->pts = from->output_frame->timeStamp;
        to->width = avctx->width;
        to->height = avctx->height;
        to->format = AV_PIX_FMT_YAMI;
        to->extended_data = to->data;
        /* XXX: put the surface id to data[3] */
        to->data[3] = reinterpret_cast<uint8_t *>(from);
        to->buf[0] = av_buffer_create((uint8_t *)from,
                                      sizeof(YamiImage),
                                      ff_yami_recycle_frame, avctx, 0);
    } else {
        ff_get_buffer(avctx, to, 0);

        to->pkt_pts = AV_NOPTS_VALUE;
        to->pkt_dts = from->output_frame->timeStamp;
        to->pts = AV_NOPTS_VALUE;
        to->width = avctx->width;
        to->height = avctx->height;
        to->format = avctx->pix_fmt;
        to->extended_data = to->data;
        ff_vaapi_get_image(from->output_frame, to);
        to->buf[3] = av_buffer_create((uint8_t *) from,
                                      sizeof(YamiImage),
                                      ff_yami_recycle_frame, avctx, 0);
    }
    return 0;
}

static const char *get_mime(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264:
        return YAMI_MIME_H264;
    case AV_CODEC_ID_HEVC:
        return YAMI_MIME_H265;
    case AV_CODEC_ID_VP8:
        return YAMI_MIME_VP8;
    case AV_CODEC_ID_MPEG2VIDEO:
        return YAMI_MIME_MPEG2;
    case AV_CODEC_ID_VC1:
        return YAMI_MIME_VC1;
    case AV_CODEC_ID_VP9:
        return YAMI_MIME_VP9;
    default:
        av_assert0(!"Invalid codec ID!");
        return NULL;
    }
}

static int yami_dec_init(AVCodecContext *avctx)
{
    YamiDecContext *s = (YamiDecContext *)avctx->priv_data;
    Decode_Status status;
    s->decoder = NULL;
    enum AVPixelFormat pix_fmts[4] =
        {
            AV_PIX_FMT_NV12,
            AV_PIX_FMT_YUV420P,
            AV_PIX_FMT_YAMI,
            AV_PIX_FMT_NONE
        };

    if (avctx->pix_fmt == AV_PIX_FMT_NONE) {
        int ret = ff_get_format(avctx, pix_fmts);
        if (ret < 0)
            return ret;

        avctx->pix_fmt = (AVPixelFormat)ret;
    }

    VADisplay va_display = ff_vaapi_create_display();
    if (!va_display) {
        av_log(avctx, AV_LOG_ERROR, "\nfail to create display\n");
        return AVERROR_BUG;
    }
    av_log(avctx, AV_LOG_VERBOSE, "yami_dec_init\n");
    const char *mime_type = get_mime(avctx->codec_id);
    s->decoder = createVideoDecoder(mime_type);
    if (!s->decoder) {
        av_log(avctx, AV_LOG_ERROR, "fail to create decoder\n");
        return AVERROR_BUG;
    }
    NativeDisplay native_display;
    native_display.type = NATIVE_DISPLAY_VA;
    native_display.handle = (intptr_t)va_display;
    s->decoder->setNativeDisplay(&native_display);

    /* set external surface allocator */
    s->p_alloc = (SurfaceAllocator *) av_mallocz(sizeof(SurfaceAllocator));
    s->p_alloc->alloc = ff_yami_alloc_surface;
    s->p_alloc->free = ff_yami_free_surface;
    s->p_alloc->unref = ff_yami_unref_surface;
    s->decoder->setAllocator(s->p_alloc);

    /* fellow h264.c style */
    if (avctx->codec_id == AV_CODEC_ID_H264) {
        if (avctx->ticks_per_frame == 1) {
            if (avctx->time_base.den < INT_MAX / 2) {
                avctx->time_base.den *= 2;
            } else
                avctx->time_base.num /= 2;
        }
        avctx->ticks_per_frame = 2;
    }

    VideoConfigBuffer config_buffer;
    memset(&config_buffer, 0, sizeof(VideoConfigBuffer));
    if (avctx->extradata && avctx->extradata_size) {
        config_buffer.data = avctx->extradata;
        config_buffer.size = avctx->extradata_size;
    }
    config_buffer.profile = VAProfileNone;
    status = s->decoder->start(&config_buffer);
    if (status != DECODE_SUCCESS && status != DECODE_FORMAT_CHANGE) {
        av_log(avctx, AV_LOG_ERROR, "yami decoder fail to start\n");
        return AVERROR_BUG;
    }
    s->in_queue = new std::deque<VideoDecodeBuffer*>;

#if HAVE_PTHREADS
    if (ff_yami_decode_thread_init(s) < 0)
        return AVERROR(ENOMEM);
#else
    av_log(avctx, AV_LOG_ERROR, "pthread libaray must be supported\n");
    return AVERROR(ENOSYS);
#endif
    s->decode_count = 0;
    s->decode_count_yami = 0;
    s->render_count = 0;
    return 0;
}

static int ff_get_best_pkt_dts(AVFrame *frame, YamiDecContext *s)
{
    if (frame->pkt_dts == AV_NOPTS_VALUE && frame->pts == AV_NOPTS_VALUE) {
        frame->pkt_dts = s->render_count * s->duration;
    }
    return 1;
}

static int yami_dec_frame(AVCodecContext *avctx, void *data,
                          int *got_frame, AVPacket *avpkt)
{
    YamiDecContext *s = (YamiDecContext *)avctx->priv_data;
    if (!s || !s->decoder)
        return -1;
    VideoDecodeBuffer *in_buffer = NULL;
    Decode_Status status = DECODE_FAIL;
    YamiImage *yami_image =  NULL;
    int ret = 0;
    AVFrame *frame = (AVFrame *)data;
    av_log(avctx, AV_LOG_VERBOSE, "yami_dec_frame\n");

    /* append packet to input buffer queue */
    in_buffer = (VideoDecodeBuffer *)av_mallocz(sizeof(VideoDecodeBuffer));
    if (!in_buffer)
        return AVERROR(ENOMEM);
    /* avoid avpkt free and data is pointer */
    if (avpkt->data && avpkt->size) {
        in_buffer->data = (uint8_t *)av_mallocz(avpkt->size);
        if (!in_buffer->data)
            return AVERROR(ENOMEM);
        memcpy(in_buffer->data, avpkt->data, avpkt->size);
    }
    in_buffer->size = avpkt->size;
    in_buffer->timeStamp = avpkt->pts;
    if (avpkt->duration != 0)
        s->duration = avpkt->duration;

    while (s->decode_status < DECODE_THREAD_GOT_EOS) {
        /* need enque eos buffer more than once */
        pthread_mutex_lock(&s->in_mutex);
        if (s->in_queue->size() < DECODE_QUEUE_SIZE) {
            s->in_queue->push_back(in_buffer);
            av_log(avctx, AV_LOG_VERBOSE, "wakeup decode thread ...\n");
            pthread_cond_signal(&s->in_cond);
            pthread_mutex_unlock(&s->in_mutex);
            break;
        }
        pthread_mutex_unlock(&s->in_mutex);
        av_log(avctx, AV_LOG_DEBUG,
               "in queue size %ld, decode count %d, decoded count %d,"
               "too many buffer are under decoding, wait ...\n",
               s->in_queue->size(), s->decode_count, s->decode_count_yami);
        av_usleep(1000);
    };
    s->decode_count++;

    /* thread status update */
    pthread_mutex_lock(&s->ctx_mutex);
    switch (s->decode_status) {
    case DECODE_THREAD_NOT_INIT:
    case DECODE_THREAD_EXIT:
        if (avpkt->data && avpkt->size) {
            s->decode_status = DECODE_THREAD_RUNING;
            pthread_create(&s->decode_thread_id, NULL, &ff_yami_decode_thread, avctx);
        }
        break;
    case DECODE_THREAD_RUNING:
        if (!avpkt->data || !avpkt->size) {
            s->decode_status = DECODE_THREAD_GOT_EOS;
            pthread_cond_signal(&s->in_cond);
        }
        break;
    case DECODE_THREAD_GOT_EOS:
        pthread_cond_signal(&s->in_cond);
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&s->ctx_mutex);

    /* get an output buffer from yami */
    do {
        if (!s->format_info) {
            av_usleep(10000);
            continue;
        }

        yami_image = (YamiImage *)av_mallocz(sizeof(YamiImage));
        if (!yami_image) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        do {
            yami_image->output_frame = s->decoder->getOutput();
            av_log(avctx, AV_LOG_DEBUG, "getoutput() status=%d\n", status);
            pthread_mutex_lock(&s->ctx_mutex);
            if (avpkt->data || yami_image->output_frame || s->decode_status == DECODE_THREAD_EXIT) {
                pthread_mutex_unlock(&s->ctx_mutex);
                break;
            }
            pthread_mutex_unlock(&s->ctx_mutex);
            av_usleep(100);
        } while (1);

        if (yami_image->output_frame) {
            yami_image->va_display = ff_vaapi_create_display();
            status = DECODE_SUCCESS;
            break;
        }
        *got_frame = 0;
        av_free(yami_image);
        return avpkt->size;
    } while (s->decode_status == DECODE_THREAD_RUNING);
    if (status != DECODE_SUCCESS) {
        av_log(avctx, AV_LOG_VERBOSE, "after processed EOS, return\n");
        return avpkt->size;
    }

    /* process the output frame */
    if (ff_convert_to_frame(avctx, yami_image, frame) < 0)
        av_log(avctx, AV_LOG_VERBOSE, "yami frame convert av_frame failed\n");
    ff_get_best_pkt_dts(frame, s);
    *got_frame = 1;
    s->render_count++;
    av_log(avctx, AV_LOG_VERBOSE,
           "decode_count_yami=%d, decode_count=%d, render_count=%d\n",
           s->decode_count_yami, s->decode_count, s->render_count);
    return avpkt->size;

fail:
    if (yami_image) {
        yami_image->output_frame.reset();
        if (yami_image)
            av_free(yami_image);
    }
    return ret;
}

static int yami_dec_close(AVCodecContext *avctx)
{
    YamiDecContext *s = (YamiDecContext *)avctx->priv_data;

    ff_yami_decode_thread_close(s);
    if (s->decoder) {
        s->decoder->stop();
        releaseVideoDecoder(s->decoder);
        s->decoder = NULL;
    }
    if (s->p_alloc)
        av_free(s->p_alloc);
    while (!s->in_queue->empty()) {
        VideoDecodeBuffer *in_buffer = s->in_queue->front();
        s->in_queue->pop_front();
        av_free(in_buffer->data);
        av_free(in_buffer);
    }
    delete s->in_queue;
    av_log(avctx, AV_LOG_VERBOSE, "yami_dec_close\n");
    return 0;
}

#define YAMI_DEC(NAME, ID) \
AVCodec ff_libyami_##NAME##_decoder = { \
    /* name */                  "libyami_" #NAME, \
    /* long_name */             NULL_IF_CONFIG_SMALL(#NAME " (libyami)"), \
    /* type */                  AVMEDIA_TYPE_VIDEO, \
    /* id */                    ID, \
    /* capabilities */          CODEC_CAP_DELAY, \
    /* supported_framerates */  NULL, \
    /* pix_fmts */              (const enum AVPixelFormat[]) { AV_PIX_FMT_YAMI, \
                                                               AV_PIX_FMT_NV12, \
                                                               AV_PIX_FMT_YUV420P, \
                                                               AV_PIX_FMT_NONE}, \
    /* supported_samplerates */ NULL, \
    /* sample_fmts */           NULL, \
    /* channel_layouts */       NULL, \
    /* max_lowres */            0, \
    /* priv_class */            NULL, \
    /* profiles */              NULL, \
    /* priv_data_size */        sizeof(YamiDecContext), \
    /* next */                  NULL, \
    /* init_thread_copy */      NULL, \
    /* update_thread_context */ NULL, \
    /* defaults */              NULL, \
    /* init_static_data */      NULL, \
    /* init */                  yami_dec_init, \
    /* encode_sub */            NULL, \
    /* encode2 */               NULL, \
    /* decode */                yami_dec_frame, \
    /* close */                 yami_dec_close, \
    /* send_frame */            NULL, \
    /* send_packet */           NULL, \
    /* receive_frame */         NULL, \
    /* receive_packet */        NULL, \
    /* flush */                 NULL, \
    /* caps_internal */         FF_CODEC_CAP_SETS_PKT_DTS, \
};

YAMI_DEC(h264, AV_CODEC_ID_H264)
YAMI_DEC(hevc, AV_CODEC_ID_HEVC)
YAMI_DEC(vp8, AV_CODEC_ID_VP8)
YAMI_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO)
YAMI_DEC(vc1, AV_CODEC_ID_VC1)
YAMI_DEC(vp9, AV_CODEC_ID_VP9)

/*
 * JACK Audio Connection Kit output device
 * Copyright (c) 
 * Author: 
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

#include "config.h"
#include <jack/jack.h>
#include <semaphore.h>
#include <string.h>
#include <unistd.h>

#include "libavutil/audio_fifo.h"
#include "libavutil/internal.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "avdevice.h"

/**
 * Size of the internal FIFO buffers as a number of audio packets
 */
#define FIFO_PACKETS_NUM 16

typedef struct JackData {
    AVClass        *class;
    jack_client_t * client;
    int             activated;
    pthread_mutex_t input_mtx;
    pthread_cond_t  input_cond;
    jack_nframes_t  sample_rate;
    jack_nframes_t  buffer_size;
    jack_port_t **  ports;
    int             nports;
    int             input_format;
    int             sample_size;
    jack_default_audio_sample_t **buffers;
    AVAudioFifo *   stream;
    float *         samples;
    int             pkt_xrun;
    int             jack_xrun;
    char *          connections;
} JackData;

static int process_callback(jack_nframes_t nframes, void *arg)
{
    /* Warning: this function runs in realtime. One mustn't allocate memory here
     * or do any other thing that could block. */

    int i, j;
    JackData *self = arg;

    if (!self->activated)
        return 0;

    if (pthread_mutex_trylock(&self->input_mtx)) {
        self->pkt_xrun = 1;
        return 0;
    }

    if (av_audio_fifo_size(self->stream) < nframes) {
        fprintf(stderr, "waiting for data %u %u.\n",
            av_audio_fifo_space(self->stream), av_audio_fifo_size(self->stream));
        self->pkt_xrun = 1;
        goto done;
    }

    for (i = 0; i < self->nports; i++)
        self->buffers[i] = jack_port_get_buffer(self->ports[i], nframes);

    /* Copy interleaved audio data from the packet into the JACK buffer */
    for (i = 0; i < nframes; i++) {
        av_audio_fifo_read(self->stream, &self->samples, 1);

        for (j = 0; j < self->nports; j++)
            self->buffers[j][i] = self->samples[j];
    }

done:
    pthread_mutex_unlock(&self->input_mtx);
    pthread_cond_signal(&self->input_cond);

    return 0;
}

static void shutdown_callback(void *arg)
{
    JackData *self = arg;
    self->client = NULL;
}

static int xrun_callback(void *arg)
{
    JackData *self = arg;
    self->jack_xrun = 1;
    return 0;
}

static int make_connections(AVFormatContext *context, JackData *self)
{
    char *conn, *conn_save;
    int test, i;

    if (!self->connections)
        return 0;

    for (i = 0;
        (conn = strtok_r(i > 0 ? NULL: self->connections, ",", &conn_save)) &&
            i < self->nports;
        ++i) {
        av_log(context, AV_LOG_DEBUG, "conn %d: %s -> %s\n", i, jack_port_name(self->ports[i]), conn);

        if ((test = jack_connect(self->client, jack_port_name(self->ports[i]), conn)) && test != EEXIST) {
            av_log(context, AV_LOG_ERROR, "Unable to make connection: %s -> %s\n", jack_port_name(self->ports[i]), conn);
            return AVERROR(EIO);
        }
    }

    return 0;
}

static int start_jack(AVFormatContext *context)
{
    JackData *self = context->priv_data;
    jack_status_t status;
    int i;

    /* Register as a JACK client, using the context url as client name. */
    self->client = jack_client_open(context->url, JackNullOption, &status);
    if (!self->client) {
        av_log(context, AV_LOG_ERROR, "Unable to register as a JACK client\n");
        return AVERROR(EIO);
    }

    pthread_mutex_init(&self->input_mtx, 0);
    pthread_cond_init(&self->input_cond, 0);

    self->sample_rate = jack_get_sample_rate(self->client);
    self->ports       = av_malloc_array(self->nports, sizeof(*self->ports));
    if (!self->ports) {
        jack_client_close(self->client);
        return AVERROR(ENOMEM);
    }
    self->buffer_size = jack_get_buffer_size(self->client);

    self->samples = av_calloc(self->nports, sizeof(*self->samples));

    /* Register JACK ports */
    for (i = 0; i < self->nports; i++) {
        char str[16];
        snprintf(str, sizeof(str), "output_%d", i + 1);
        self->ports[i] = jack_port_register(self->client, str,
                                            JACK_DEFAULT_AUDIO_TYPE,
                                            JackPortIsOutput, 0);
        if (!self->ports[i]) {
            av_log(context, AV_LOG_ERROR, "Unable to register port %s:%s\n",
                   context->url, str);
            jack_client_close(self->client);
            return AVERROR(EIO);
        }
    }

    /* Register JACK callbacks */
    jack_set_process_callback(self->client, process_callback, self);
    jack_on_shutdown(self->client, shutdown_callback, self);
    jack_set_xrun_callback(self->client, xrun_callback, self);

    /* Create FIFO buffer */
    self->stream = av_audio_fifo_alloc(self->input_format, self->nports, self->buffer_size);
    if (!self->stream) {
        jack_client_close(self->client);
        return AVERROR(ENOMEM);
    }

    return 0;
}

static void stop_jack(JackData *self)
{
    int i;

    if (self->client) {
        if (self->activated)
            jack_deactivate(self->client);
        jack_client_close(self->client);
    }
    free(self->samples);
    pthread_cond_destroy(&self->input_cond);
    pthread_mutex_destroy(&self->input_mtx);
    av_audio_fifo_free(self->stream);
    av_freep(&self->ports);
}

static int audio_write_header(AVFormatContext *context)
{
    JackData *self = context->priv_data;
    AVStream *stream;
    int test;

    stream = context->streams[0];

    self->input_format  = stream->codecpar->format;
    self->sample_size   = av_get_bytes_per_sample(stream->codecpar->format);
    self->nports        = stream->codecpar->channels;
    self->buffers       = malloc(self->nports * sizeof(*self->buffers));

    if ((test = start_jack(context)))
        return test;

    return 0;
}

static int audio_write_packet(AVFormatContext *context, AVPacket *pkt)
{
    JackData *self = context->priv_data;
    int nsamples = 0;

    if (self->pkt_xrun) {
        av_log(context, AV_LOG_WARNING, "Audio packet xrun\n");
        self->pkt_xrun = 0;
    }

    if (self->jack_xrun) {
        av_log(context, AV_LOG_WARNING, "JACK xrun\n");
        self->jack_xrun = 0;
    }

    if (pkt->size < self->sample_size * self->nports) {
        av_log(context, AV_LOG_WARNING, "ff xrun\n");
        return 0;
    }

    nsamples = pkt->size / self->sample_size / self->nports;

    if (self->activated) {
        /* Wait for a packet coming back from process_callback(), if one isn't
         * available yet */
        pthread_mutex_lock(&self->input_mtx);

        while (av_audio_fifo_size(self->stream) >= self->buffer_size)
            pthread_cond_wait(&self->input_cond, &self->input_mtx);
    }

    /* queue the audio data for process_callback() */
    av_audio_fifo_write(self->stream, (void **)&pkt->data, nsamples);

    /* Activate the JACK client on first packet write. Activating the JACK client
     * means that process_callback() starts to get called at regular interval.
     * If we activate it in audio_read_header(), we're actually reading audio data
     * from the device before instructed to, and that may result in an overrun. */
    if (!self->activated) {
        if (!jack_activate(self->client)) {
            self->activated = 1;
            av_log(context, AV_LOG_INFO,
                   "JACK client registered and activated (rate=%dHz, buffer_size=%d frames)\n",
                   self->sample_rate, self->buffer_size);
        } else {
            av_log(context, AV_LOG_ERROR, "Unable to activate JACK client\n");
            return AVERROR(EIO);
        }

        if (make_connections(context, self))
            return AVERROR(EIO);
    }

    pthread_mutex_unlock(&self->input_mtx);

    return 0;
}

static int audio_write_trailer(AVFormatContext *context)
{
    JackData *self = context->priv_data;
    stop_jack(self);
    return 0;
}

#define OFFSET(x) offsetof(JackData, x)
static const AVOption options[] = {
    { "connections", "List of jack output connections to make, in channel order.", OFFSET(connections), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_ENCODING_PARAM },
    { NULL },
};

static const AVClass jack_outdev_class = {
    .class_name     = "JACK outdev",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
    .category       = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
};

AVOutputFormat ff_jack_muxer = {
    .name           = "jack",
    .long_name      = NULL_IF_CONFIG_SMALL("JACK Audio Connection Kit Output"),
    .priv_data_size = sizeof(JackData),
    .audio_codec    = AV_NE(AV_CODEC_ID_PCM_F32BE, AV_CODEC_ID_PCM_F32LE),
    .video_codec    = AV_CODEC_ID_NONE,
    .write_header   = audio_write_header,
    .write_packet   = audio_write_packet,
    .write_trailer  = audio_write_trailer,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &jack_outdev_class,
};

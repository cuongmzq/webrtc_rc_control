/*
Copyright (c) 2012, Broadcom Europe Ltd
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "mmalcam.h"

#include "interface/mmal/mmal_logging.h"

#define VIEWFINDER_LAYER 2
#define DEFAULT_VIDEO_FORMAT "1280x720:h264";
#define DEFAULT_BIT_RATE 5000000
#define DEFAULT_CAM_NUM 0

struct
{
    const char *name;
    MMALCAM_CHANGE_T value;
} mmalcam_change_table[] = {
    {"image_effect", MMALCAM_CHANGE_IMAGE_EFFECT},
    {"rotation", MMALCAM_CHANGE_ROTATION},
    {"zoom", MMALCAM_CHANGE_ZOOM},
    {"focus", MMALCAM_CHANGE_FOCUS},
    {"drc", MMALCAM_CHANGE_DRC},
    {"hdr", MMALCAM_CHANGE_HDR},
    {"contrast", MMALCAM_CHANGE_CONTRAST},
    {"brightness", MMALCAM_CHANGE_BRIGHTNESS},
    {"saturation", MMALCAM_CHANGE_SATURATION},
    {"sharpness", MMALCAM_CHANGE_SHARPNESS},
};

static int stop;
static VCOS_THREAD_T camcorder_thread;
static MMALCAM_BEHAVIOUR_T camcorder_behaviour;
static uint32_t sleepy_time;
static MMAL_BOOL_T stopped_already;

/*****************************************************************************/
int start_mmalcam(struct mmalcam_args &args) {
    VCOS_THREAD_ATTR_T attrs;
    VCOS_STATUS_T status;
    int result = 0;

    vcos_log_register("mmalcam", VCOS_LOG_CATEGORY);
    printf("MMAL Camera Test App\n");
    signal(SIGINT, signal_handler);
    camcorder_behaviour = *(args.id);

    camcorder_behaviour.layer = VIEWFINDER_LAYER;
    // camcorder_behaviour.vformat = DEFAULT_VIDEO_FORMAT;
    camcorder_behaviour.zero_copy = 1;
    // camcorder_behaviour.bit_rate = DEFAULT_BIT_RATE;
    camcorder_behaviour.focus_test = MMAL_PARAM_FOCUS_MAX;
    camcorder_behaviour.camera_num = DEFAULT_CAM_NUM;

    status = vcos_semaphore_create(&camcorder_behaviour.init_sem, "mmalcam-init", 0);
    vcos_assert(status == VCOS_SUCCESS);

    vcos_thread_attr_init(&attrs);

    if (vcos_thread_create(&camcorder_thread, "mmal camcorder", &attrs, &mmal_camcorder, &args) != VCOS_SUCCESS)
    {
        LOG_ERROR("Thread creation failure");
        result = -2;
        return show_error(&result);
    }

    vcos_semaphore_wait(&camcorder_behaviour.init_sem);
    if (camcorder_behaviour.init_result != MMALCAM_INIT_SUCCESS)
    {
        LOG_ERROR("Initialisation failed: %d", camcorder_behaviour.init_result);
        result = (int)camcorder_behaviour.init_result;
        return show_error(&result);
    }

    if (sleepy_time != 0)
    {
        sleep(sleepy_time);
        stop = 1;
    }
    return show_error(&result);
}

/*****************************************************************************/
static void signal_handler(int signum)
{
    (void)signum;

    if (stopped_already)
    {
        LOG_ERROR("Killing program");
        exit(255);
    }
    else
    {
        LOG_ERROR("Stopping normally. CTRL+C again to kill program");
        stop = 1;
        stopped_already = 1;
    }
}

/*****************************************************************************/
static void mmalcam_dump_stats(const char *title, MMAL_PARAMETER_STATISTICS_T *stats)
{
    printf("[%s]\n", title);
    printf("buffer_count: %u\n", stats->buffer_count);
    printf("frame_count: %u\n", stats->frame_count);
    printf("frames_skipped: %u\n", stats->frames_skipped);
    printf("frames_discarded: %u\n", stats->frames_discarded);
    printf("eos_seen: %u\n", stats->eos_seen);
    printf("maximum_frame_bytes: %u\n", stats->maximum_frame_bytes);
    printf("total_bytes_hi: %u\n", (uint32_t)(stats->total_bytes >> 32));
    printf("total_bytes_lo: %u\n", (uint32_t)(stats->total_bytes));
    printf("corrupt_macroblocks: %u\n", stats->corrupt_macroblocks);
}

static int show_error(const int *status)
{
    LOG_TRACE("Waiting for camcorder thread to terminate");
    vcos_thread_join(&camcorder_thread, NULL);

    mmalcam_dump_stats("Render", &camcorder_behaviour.render_stats);
    if (camcorder_behaviour.uri)
        mmalcam_dump_stats("Encoder", &camcorder_behaviour.encoder_stats);

    vcos_semaphore_delete(&camcorder_behaviour.init_sem);
    return *status;
}

/*****************************************************************************/
static void *mmal_camcorder(struct mmalcam_args *args)
{
    MMALCAM_BEHAVIOUR_T *behaviour = args->id;
    int value;

    value = mmal_start_camcorder(&stop, behaviour, args->cb);

    LOG_TRACE("Thread terminating, result %d", value);
    return (void *)(uintptr_t)value;
}
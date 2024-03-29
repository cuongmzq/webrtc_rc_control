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
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdarg.h>

#include "mmalcam.h"
#include "viewfinder.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_default_components.h"

#include "config.h"

/** Number of buffers we want to use for video render. Video render needs at least 2. */
#define VIDEO_OUTPUT_BUFFERS_NUM 3

/** Initialise a parameter structure */
#define INIT_PARAMETER(PARAM, PARAM_ID)   \
   do {                                   \
      memset(&(PARAM), 0, sizeof(PARAM)); \
      (PARAM).hdr.id = PARAM_ID;          \
      (PARAM).hdr.size = sizeof(PARAM);   \
   } while (0)

/* Utility function to create and setup the camera viewfinder component */
static MMAL_COMPONENT_T *test_camera_create(MMALCAM_BEHAVIOUR_T *behaviour, MMAL_STATUS_T *status);
static MMAL_BOOL_T mmalcam_next_effect(MMAL_COMPONENT_T *camera);
static MMAL_BOOL_T mmalcam_next_rotation(MMAL_COMPONENT_T *camera);
static MMAL_BOOL_T mmalcam_next_zoom(MMAL_COMPONENT_T *camera);
static MMAL_BOOL_T mmalcam_next_focus(MMAL_COMPONENT_T *camera);
static MMAL_BOOL_T mmalcam_reset_focus(MMAL_COMPONENT_T *camera, MMAL_PARAM_FOCUS_T focus_setting);
static MMAL_BOOL_T mmalcam_next_drc(MMAL_COMPONENT_T *camera);
static MMAL_BOOL_T mmalcam_next_hdr(MMAL_COMPONENT_T *camera);
static MMAL_BOOL_T mmalcam_next_colour_param(MMAL_COMPONENT_T *camera, uint32_t id, int min, int max, const char *param_name);

/* Utility function to create and setup the video encoder component */
static MMAL_COMPONENT_T *test_video_encoder_create(MMALCAM_BEHAVIOUR_T *behaviour, MMAL_STATUS_T *status);

/*****************************************************************************/

typedef enum {
   MMAL_CAM_BUFFER_READY         = 1 << 0,
   MMAL_CAM_AUTOFOCUS_COMPLETE   = 1 << 1,
   MMAL_CAM_ANY_EVENT            = 0x7FFFFFFF
} MMAL_CAM_EVENT_T;

static VCOS_EVENT_FLAGS_T events;
VCOS_LOG_CAT_T mmalcam_log_category;
static MMAL_BOOL_T zero_copy;
static MMAL_BOOL_T tunneling;

static MMAL_BOOL_T enable_zero_copy(void)
{
   return zero_copy;
}

static MMAL_BOOL_T enable_tunneling(void)
{
   return tunneling;
}

/* Buffer header callbacks */
static void control_bh_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   LOG_DEBUG("control_bh_cb %p,%p (cmd=0x%08x)", port, buffer, buffer->cmd);
   if (buffer->cmd == MMAL_EVENT_PARAMETER_CHANGED)
   {
      MMAL_EVENT_PARAMETER_CHANGED_T *param = (MMAL_EVENT_PARAMETER_CHANGED_T *)buffer->data;

      vcos_assert(buffer->length >= sizeof(MMAL_EVENT_PARAMETER_CHANGED_T));
      vcos_assert(buffer->length == param->hdr.size);
      switch (param->hdr.id)
      {
         case MMAL_PARAMETER_FOCUS_STATUS:
            vcos_assert(param->hdr.size == sizeof(MMAL_PARAMETER_FOCUS_STATUS_T));
            {
               MMAL_PARAMETER_FOCUS_STATUS_T *focus_status = (MMAL_PARAMETER_FOCUS_STATUS_T *)param;
               LOG_INFO("Focus status: %d", focus_status->status);
               vcos_event_flags_set(&events, MMAL_CAM_AUTOFOCUS_COMPLETE, VCOS_OR);
            }
            break;
         case MMAL_PARAMETER_CAMERA_NUM:
            vcos_assert(param->hdr.size == sizeof(MMAL_PARAMETER_UINT32_T));
            {
               MMAL_PARAMETER_UINT32_T *camera_num = (MMAL_PARAMETER_UINT32_T *)param;
               LOG_INFO("Camera number: %d", camera_num->value);
            }
            break;
         default:
            LOG_ERROR("Unexpected changed event for parameter 0x%08x", param->hdr.id);
      }
   }
   else
   {
      LOG_ERROR("Unexpected event, 0x%08x", buffer->cmd);
   }
   mmal_buffer_header_release(buffer);
}

static void generic_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   if (buffer->cmd != 0)
   {
      LOG_INFO("%s callback: event %u not supported", port->name, buffer->cmd);
      mmal_buffer_header_release(buffer);
   }
   else
   {
      MMAL_QUEUE_T *queue = (MMAL_QUEUE_T *)port->userdata;

      LOG_DEBUG("%s callback", port->name);
      mmal_queue_put(queue, buffer);
   }

   vcos_event_flags_set(&events, MMAL_CAM_BUFFER_READY, VCOS_OR);
}

static void generic_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   if (buffer->cmd != 0)
   {
      LOG_INFO("%s callback: event %u not supported", port->name, buffer->cmd);
   }

   mmal_buffer_header_release(buffer);
   vcos_event_flags_set(&events, MMAL_CAM_BUFFER_READY, VCOS_OR);
}

static MMAL_STATUS_T setup_output_port(MMAL_PORT_T *output_port, MMAL_QUEUE_T **p_queue, MMAL_POOL_T **p_pool)
{
   MMAL_STATUS_T status = MMAL_ENOMEM;
   MMAL_QUEUE_T *queue = NULL;
   MMAL_POOL_T *pool = NULL;

   /* Create a queue for frames filled by the output port.
    * The main loop will pass these on to the input port. */
   queue = mmal_queue_create();
   if (!queue)
   {
      LOG_ERROR("failed to create queue for %s", output_port->name);
      goto error;
   }

   /* Create pool of buffer headers for the output port to consume */
   pool = mmal_port_pool_create(output_port, output_port->buffer_num, output_port->buffer_size);
   if (!pool)
   {
      LOG_ERROR("failed to create pool for %s", output_port->name);
      goto error;
   }

   output_port->userdata = (void *)queue;

   status = mmal_port_enable(output_port, generic_output_port_cb);
   if (status != MMAL_SUCCESS)
   {
      LOG_ERROR("failed to enable %s", output_port->name);
      goto error;
   }

   *p_queue = queue;
   *p_pool = pool;

   return MMAL_SUCCESS;

error:
   if (queue)
      mmal_queue_destroy(queue);
   if (pool)
      mmal_pool_destroy(pool);

   return status;
}

static MMAL_STATUS_T connect_ports(MMAL_PORT_T *output_port, MMAL_PORT_T *input_port, MMAL_QUEUE_T **p_queue, MMAL_POOL_T **p_pool)
{
   MMAL_STATUS_T status;

   status = mmal_format_full_copy(input_port->format, output_port->format);
   if (status != MMAL_SUCCESS)
      return status;

   status = mmal_port_format_commit(input_port);
   if (status != MMAL_SUCCESS)
      return status;

   if (enable_tunneling())
   {
      status = mmal_port_connect(output_port, input_port);
      if (status != MMAL_SUCCESS)
         return status;

      status = mmal_port_enable(output_port, NULL);
      if (status != MMAL_SUCCESS)
         mmal_port_disconnect(output_port);

      return status;
   }

   /* Non-tunneling setup */
   input_port->buffer_size = input_port->buffer_size_recommended;
   if (input_port->buffer_size < input_port->buffer_size_min)
      input_port->buffer_size = input_port->buffer_size_min;
   input_port->buffer_num = input_port->buffer_num_recommended;
   if (input_port->buffer_num < input_port->buffer_num_min)
      input_port->buffer_num = input_port->buffer_num_min;
   output_port->buffer_size = output_port->buffer_size_recommended;
   if (output_port->buffer_size < output_port->buffer_size_min)
      output_port->buffer_size = output_port->buffer_size_min;
   output_port->buffer_num = output_port->buffer_num_recommended;
   if (output_port->buffer_num < output_port->buffer_num_min)
      output_port->buffer_num = output_port->buffer_num_min;

   input_port->buffer_num = output_port->buffer_num =
      MMAL_MAX(input_port->buffer_num, output_port->buffer_num);
   input_port->buffer_size = output_port->buffer_size =
      MMAL_MAX(input_port->buffer_size, output_port->buffer_size);

   status = setup_output_port(output_port, p_queue, p_pool);
   if (status != MMAL_SUCCESS)
      goto error;

   status = mmal_port_enable(input_port, generic_input_port_cb);
   if (status != MMAL_SUCCESS)
      goto error;

   return status;

error:
   if (input_port->is_enabled)
      mmal_port_disable(input_port);
   if (output_port->is_enabled)
      mmal_port_disable(output_port);
   if (*p_pool)
      mmal_pool_destroy(*p_pool);
   if (*p_queue)
      mmal_queue_destroy(*p_queue);

   return status;
}

static MMAL_STATUS_T send_buffer_from_queue(MMAL_PORT_T *port, MMAL_QUEUE_T *queue)
{
   MMAL_STATUS_T status = MMAL_SUCCESS;
   MMAL_BUFFER_HEADER_T *buffer;

   if (!queue)
      return MMAL_SUCCESS;

   buffer = mmal_queue_get(queue);

   if (buffer)
   {
      status = mmal_port_send_buffer(port, buffer);

      if (status != MMAL_SUCCESS)
      {
         mmal_queue_put_back(queue, buffer);
         LOG_DEBUG("%s send failed (%i)", port->name, status);
      }
   }

   return status;
}

static MMAL_STATUS_T fill_port_from_pool(MMAL_PORT_T *port, MMAL_POOL_T *pool)
{
   MMAL_STATUS_T status = MMAL_SUCCESS;
   MMAL_QUEUE_T *queue;

   if (!pool)
      return MMAL_SUCCESS;

   queue = pool->queue;
   while (status == MMAL_SUCCESS && mmal_queue_length(queue) > 0)
      status = send_buffer_from_queue(port, queue);

   return status;
}

static void disable_port(MMAL_PORT_T *port)
{
   if (port && port->is_enabled)
      mmal_port_disable(port);
}


static int parse_vformat(const char* vformat, uint32_t *out_width,
      uint32_t *out_height, uint32_t *out_encoding)
{
   char vcodec[8];
   uint32_t width, height, encoding;

   // coverity[secure_coding] Scanning integer values, and a string where the length is safe given vcodec declaration
   if (sscanf(vformat, "%4ux%4u:%7s", &width, &height, vcodec) != 3)
   {
      fprintf(stderr, "Error, malformed or unsupported video format: %s\n", vformat);
      return -1;
   }

   if (!vcos_strncasecmp(vcodec, "h263", 4))
   {
      encoding = MMAL_ENCODING_H263;
      /* Special case, H263 supports a limited set of resolutions */
      if (!((width ==  128 && height ==   96) ||
            (width ==  176 && height ==  144) ||
            (width ==  352 && height ==  288) ||
            (width ==  704 && height ==  576) ||
            (width == 1408 && height == 1152)))
      {
         fprintf(stderr,
               "Error, only 128x96, 176x144, 352x288, 704x576 and 1408x1152 are supported for H263\n");
         return -1;
      }
   }
   else if (!vcos_strncasecmp(vcodec, "mp4v", 4))
      encoding = MMAL_ENCODING_MP4V;
   else if (!vcos_strncasecmp(vcodec, "h264", 4))
      encoding = MMAL_ENCODING_H264;
   else if (!vcos_strncasecmp(vcodec, "jpeg", 4))
      encoding = MMAL_ENCODING_JPEG;
   else
   {
      fprintf(stderr, "Error, unknown video encoding: %s\n", vcodec);
      return -1;
   }

   if (out_width)
      *out_width = width;
   if (out_height)
      *out_height = height;
   if (out_encoding)
      *out_encoding = encoding;
   LOG_DEBUG("Video format: w:%d h:%d codec:%4.4s", width, height, (const char *)&encoding);

   return 0;
}

/*****************************************************************************/
static MMAL_COMPONENT_T *test_camera_create(MMALCAM_BEHAVIOUR_T *behaviour, MMAL_STATUS_T *status)
{
   MMAL_COMPONENT_T *camera = 0;
   MMAL_ES_FORMAT_T *format;
   MMAL_PARAMETER_CHANGE_EVENT_REQUEST_T change_event_request =
         {{MMAL_PARAMETER_CHANGE_EVENT_REQUEST, sizeof(MMAL_PARAMETER_CHANGE_EVENT_REQUEST_T)}, 0, 1};
   MMAL_PORT_T *viewfinder_port = NULL, *video_port = NULL, *still_port = NULL;
   uint32_t width, height;
   MMAL_PARAMETER_INT32_T camera_num =
         {{MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)},0};

   /* Create the component */
   *status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);
   if(*status != MMAL_SUCCESS)
   {
      LOG_ERROR("couldn't create camera");
      goto error;
   }
   if(!camera->output_num)
   {
      LOG_ERROR("camera doesn't have output ports");
      *status = MMAL_EINVAL;
      goto error;
   }

   viewfinder_port = camera->output[0];
   video_port = camera->output[1];
   still_port = camera->output[2];

   change_event_request.change_id = MMAL_PARAMETER_FOCUS_STATUS;
   *status = mmal_port_parameter_set(camera->control, &change_event_request.hdr);
   if (*status != MMAL_SUCCESS && *status != MMAL_ENOSYS)
   {
      LOG_ERROR("No focus status change events");
   }
   camera_num.value = behaviour->camera_num;
   *status = mmal_port_parameter_set(camera->control, &camera_num.hdr);
   if (*status != MMAL_SUCCESS && *status != MMAL_ENOSYS)
   {
       LOG_ERROR("No camera number change events");
   }
   if (enable_zero_copy())
   {
      MMAL_PARAMETER_BOOLEAN_T param_zc =
         {{MMAL_PARAMETER_ZERO_COPY, sizeof(MMAL_PARAMETER_BOOLEAN_T)}, 1};
      *status = mmal_port_parameter_set(viewfinder_port, &param_zc.hdr);
      if( *status != MMAL_SUCCESS && *status != MMAL_ENOSYS )
      {
         LOG_ERROR("failed to set zero copy on camera output");
         goto error;
      }
      LOG_INFO("enabled zero copy on camera");
      *status = mmal_port_parameter_set(video_port, &param_zc.hdr);
      if( *status != MMAL_SUCCESS && *status != MMAL_ENOSYS )
      {
         LOG_ERROR("failed to set zero copy on camera output");
         goto error;
      }
      *status = mmal_port_parameter_set(still_port, &param_zc.hdr);
      if( *status != MMAL_SUCCESS && *status != MMAL_ENOSYS )
      {
         LOG_ERROR("failed to set zero copy on camera output");
         goto error;
      }
   }

   if ( behaviour->change == MMALCAM_CHANGE_HDR )
   {
      MMAL_PARAMETER_ALGORITHM_CONTROL_T algo_ctrl = {{MMAL_PARAMETER_ALGORITHM_CONTROL, sizeof(MMAL_PARAMETER_ALGORITHM_CONTROL_T)},
                        MMAL_PARAMETER_ALGORITHM_CONTROL_ALGORITHMS_HIGH_DYNAMIC_RANGE, 1 };
      mmal_port_parameter_set(camera->control, &algo_ctrl.hdr);
   }

   *status = mmal_port_enable(camera->control, control_bh_cb);
   if (*status)
   {
      LOG_ERROR("control port couldn't be enabled: %d", *status);
      goto error;
   }

   /* Set camera viewfinder port format */
   if (parse_vformat(behaviour->vformat, &width, &height, NULL))
   {
      *status = MMAL_EINVAL;
      goto error;
   }

   /* Default to integer frame rate in numerator */
   if (!behaviour->frame_rate.den)
      behaviour->frame_rate.den = 1;

   {
      MMAL_PARAMETER_CAMERA_CONFIG_T cam_config = {{MMAL_PARAMETER_CAMERA_CONFIG,sizeof(cam_config)},
                              .max_stills_w =      width,
                              .max_stills_h =      height,
                              .stills_yuv422 =     0,
                              .one_shot_stills =   0,
                              .max_preview_video_w = width,
                              .max_preview_video_h = height,
                              .num_preview_video_frames = 3,
                              .stills_capture_circular_buffer_height = 0,
                              .fast_preview_resume = 0,
                                          /* No way of using fast resume in Android, as preview
                                           * automatically stops on capture.
                                           */
                              .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
                              };

      mmal_port_parameter_set(camera->control, &cam_config.hdr);
   }

   /* Set up the viewfinder port format */
   format = viewfinder_port->format;
   if (behaviour->opaque)
      format->encoding = MMAL_ENCODING_OPAQUE;
   else
      format->encoding = MMAL_ENCODING_I420;

   format->es->video.width = width;
   format->es->video.height = height;
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = width;
   format->es->video.crop.height = height;
   format->es->video.frame_rate = behaviour->frame_rate;

//    *status = mmal_port_format_commit(viewfinder_port);
//    if(*status)
//    {
//       LOG_ERROR("camera viewfinder format couldn't be set");
//       goto error;
//    }

   /* Set the same format on the video (for encoder) port */
   mmal_format_full_copy(video_port->format, format);
   *status = mmal_port_format_commit(video_port);
   if(*status)
   {
      LOG_ERROR("camera video format couldn't be set");
      goto error;
   }

   /* Ensure there are enough buffers to avoid dropping frames */
   if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

//    /* Set the same format on the still (for encoder) port */
//    mmal_format_full_copy(still_port->format, format);
//    *status = mmal_port_format_commit(still_port);
//    if(*status)
//    {
//       LOG_ERROR("camera still format couldn't be set");
//       goto error;
//    }

//    /* Ensure there are enough buffers to avoid dropping frames */
//    if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
//       still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

   /* Enable component */
   *status = mmal_component_enable(camera);
   if(*status)
   {
      LOG_ERROR("camera component couldn't be enabled");
      goto error;
   }

   return camera;

 error:
   if(camera) mmal_component_destroy(camera);
   return 0;
}

/*****************************************************************************/
static MMAL_BOOL_T mmalcam_next_effect(MMAL_COMPONENT_T *camera)
{
   static const MMAL_PARAM_IMAGEFX_T effects[] = {
               MMAL_PARAM_IMAGEFX_NONE,
               MMAL_PARAM_IMAGEFX_NEGATIVE,
               MMAL_PARAM_IMAGEFX_SOLARIZE
            };
   static unsigned int index;
   MMAL_PARAMETER_IMAGEFX_T image_fx = {{ MMAL_PARAMETER_IMAGE_EFFECT, sizeof(image_fx)},0};
   MMAL_PARAMETER_IMAGEFX_T image_fx_check = {{ MMAL_PARAMETER_IMAGE_EFFECT, sizeof(image_fx)},0};
   MMAL_STATUS_T result;

   index++;
   if(index >= countof(effects))
      index = 0;
   image_fx.value = effects[index];
   result = mmal_port_parameter_set(camera->control, &image_fx.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to set image effect, %d", result);
      return MMAL_FALSE;
   }
   result = mmal_port_parameter_get(camera->control, &image_fx_check.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to retrieve image effect, %d", result);
      return MMAL_FALSE;
   }
   if (memcmp(&image_fx, &image_fx_check, sizeof(image_fx)) != 0)
   {
      LOG_ERROR("Image effect set (%d) was not retrieved (%d)", image_fx.value, image_fx_check.value);
      return MMAL_FALSE;
   }
   return MMAL_TRUE;
}

/*****************************************************************************/
static MMAL_BOOL_T mmalcam_next_rotation(MMAL_COMPONENT_T *camera)
{
   static MMAL_PARAMETER_UINT32_T rotate = {{MMAL_PARAMETER_ROTATION,sizeof(rotate)},0};
   MMAL_PARAMETER_UINT32_T rotate_check = {{MMAL_PARAMETER_ROTATION,sizeof(rotate_check)},0};
   MMAL_STATUS_T result;

   rotate.value += 90;
   if(rotate.value == 360)
      rotate.value = 0;
   result = mmal_port_parameter_set(camera->output[0], &rotate.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to set rotation, %d", result);
      return MMAL_FALSE;
   }
   result = mmal_port_parameter_get(camera->output[0], &rotate_check.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to retrieve rotation, %d", result);
      return MMAL_FALSE;
   }
   if (memcmp(&rotate, &rotate_check, sizeof(rotate)) != 0)
   {
      LOG_ERROR("Rotation set (%d) was not retrieved (%d)", rotate.value, rotate_check.value);
      return MMAL_FALSE;
   }
   return MMAL_TRUE;
}

/*****************************************************************************/
static MMAL_BOOL_T mmalcam_next_zoom(MMAL_COMPONENT_T *camera)
{
   static MMAL_PARAMETER_SCALEFACTOR_T scale = {{MMAL_PARAMETER_ZOOM,sizeof(scale)},1<<16,1<<16};
   static int32_t dirn = 1 << 14;
   MMAL_PARAMETER_SCALEFACTOR_T scale_check = {{MMAL_PARAMETER_ZOOM,sizeof(scale_check)},0,0};
   MMAL_STATUS_T result;

   scale.scale_x += dirn;
   scale.scale_y += dirn;
   if (scale.scale_x >= 4<<16 || scale.scale_x <= 1<<16)
      dirn = -dirn;
   result = mmal_port_parameter_set(camera->control, &scale.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to set scale, %d", result);
      return MMAL_FALSE;
   }
   result = mmal_port_parameter_get(camera->control, &scale_check.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to retrieve scale, %d", result);
      return MMAL_FALSE;
   }
   if (memcmp(&scale, &scale_check, sizeof(scale)) != 0)
   {
      LOG_ERROR("Scale set (%d,%d) was not retrieved (%d,%d)",
            scale.scale_x, scale.scale_y, scale_check.scale_x, scale_check.scale_y);
      return MMAL_FALSE;
   }
   return MMAL_TRUE;
}

/*****************************************************************************/
static MMAL_BOOL_T mmalcam_next_focus(MMAL_COMPONENT_T *camera)
{
   static const MMAL_PARAM_FOCUS_T focus_setting[] = {
               MMAL_PARAM_FOCUS_AUTO,
               MMAL_PARAM_FOCUS_AUTO_MACRO,
               MMAL_PARAM_FOCUS_CAF,
               MMAL_PARAM_FOCUS_FIXED_INFINITY,
               MMAL_PARAM_FOCUS_FIXED_HYPERFOCAL,
               MMAL_PARAM_FOCUS_FIXED_MACRO,
               MMAL_PARAM_FOCUS_EDOF,
            };
   static unsigned int index;
   static MMAL_PARAMETER_FOCUS_T focus = {{MMAL_PARAMETER_FOCUS,sizeof(focus)},0};
   static MMAL_PARAMETER_FOCUS_T focus_check = {{MMAL_PARAMETER_FOCUS,sizeof(focus)},0};
   MMAL_STATUS_T result;

   index++;
   if(index >= countof(focus_setting))
      index = MMAL_FALSE;
   focus.value = focus_setting[index];
   result = mmal_port_parameter_set(camera->control, &focus.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to set focus to %d", focus.value);
      /* As this depends on the camera module, do not fail */
      return MMAL_TRUE;
   }
   result = mmal_port_parameter_get(camera->control, &focus_check.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to retrieve focus, %d", result);
      return MMAL_FALSE;
   }
   /* Focus setting is asynchronous, so the value read back may not match what was set */
   return MMAL_TRUE;
}

/*****************************************************************************/
static MMAL_BOOL_T mmalcam_reset_focus(MMAL_COMPONENT_T *camera, MMAL_PARAM_FOCUS_T focus_setting)
{
   MMAL_PARAMETER_FOCUS_T focus = {{MMAL_PARAMETER_FOCUS, sizeof(focus)},MMAL_PARAM_FOCUS_FIXED_HYPERFOCAL};
   MMAL_STATUS_T result;

   result = mmal_port_parameter_set(camera->control, &focus.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to set focus to HYPERFOCAL, result %d", result);
      return MMAL_FALSE;
   }
   focus.value = focus_setting;
   result = mmal_port_parameter_set(camera->control, &focus.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to set focus to %d, result %d", focus_setting, result);
      return MMAL_FALSE;
   }
   return MMAL_TRUE;
}

/*****************************************************************************/
static MMAL_BOOL_T mmalcam_next_drc(MMAL_COMPONENT_T *camera)
{
   static const MMAL_PARAMETER_DRC_STRENGTH_T drc_setting[] = {
               MMAL_PARAMETER_DRC_STRENGTH_OFF,
               MMAL_PARAMETER_DRC_STRENGTH_LOW,
               MMAL_PARAMETER_DRC_STRENGTH_MEDIUM,
               MMAL_PARAMETER_DRC_STRENGTH_HIGH
            };
   static unsigned int index;
   MMAL_STATUS_T result;
   MMAL_PARAMETER_DRC_T drc = {{MMAL_PARAMETER_DYNAMIC_RANGE_COMPRESSION,sizeof(drc)},0};
   MMAL_PARAMETER_DRC_T drc_check = {{MMAL_PARAMETER_DYNAMIC_RANGE_COMPRESSION,sizeof(drc_check)},0};

   index++;
   if(index >= countof(drc_setting))
      index = 0;
   drc.strength = drc_setting[index];

   result = mmal_port_parameter_set(camera->control, &drc.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to set drc, %d", result);
      return MMAL_FALSE;
   }
   result = mmal_port_parameter_get(camera->control, &drc_check.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to retrieve drc, %d", result);
      return MMAL_FALSE;
   }
   if (memcmp(&drc, &drc_check, sizeof(drc)) != 0)
   {
      LOG_ERROR("DRC set (%d) was not retrieved (%d)", drc.strength, drc_check.strength);
      return MMAL_FALSE;
   }
   return MMAL_TRUE;
}

/*****************************************************************************/
static MMAL_BOOL_T mmalcam_next_hdr(MMAL_COMPONENT_T *camera)
{
   static const MMAL_BOOL_T hdr_setting[] = {
               MMAL_FALSE,
               MMAL_TRUE,
            };
   static unsigned int index;
   MMAL_STATUS_T result;
   MMAL_PARAMETER_BOOLEAN_T hdr = {{MMAL_PARAMETER_HIGH_DYNAMIC_RANGE,sizeof(hdr)},0};
   MMAL_PARAMETER_BOOLEAN_T hdr_check = {{MMAL_PARAMETER_HIGH_DYNAMIC_RANGE,sizeof(hdr_check)},0};

   index++;
   if(index >= countof(hdr_setting))
      index = 0;
   hdr.enable = hdr_setting[index];

   result = mmal_port_parameter_set(camera->control, &hdr.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to set hdr, %d", result);
      return MMAL_FALSE;
   }
   result = mmal_port_parameter_get(camera->control, &hdr_check.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to retrieve hdr, %d", result);
      return MMAL_FALSE;
   }
   if (memcmp(&hdr, &hdr_check, sizeof(hdr)) != 0)
   {
      LOG_ERROR("HDR set (%d) was not retrieved (%d)", hdr.enable, hdr_check.enable);
      return MMAL_FALSE;
   }
   return MMAL_TRUE;
}

/*****************************************************************************/
/* Contrast, brightness, saturation, and sharpness all take the same format,
 * but need different parameter IDs, and brightness is 0-100, not -100 to 100.
 */
static MMAL_BOOL_T mmalcam_next_colour_param(MMAL_COMPONENT_T *camera, uint32_t id, int min, int max, const char *param_name)
{
   static MMAL_PARAMETER_RATIONAL_T param = {{MMAL_PARAMETER_GROUP_CAMERA,sizeof(param)},{0,100}};
   MMAL_PARAMETER_RATIONAL_T param_check = {{MMAL_PARAMETER_GROUP_CAMERA,sizeof(param_check)},{0,100}};
   MMAL_STATUS_T result;
   param.hdr.id = id;
   param_check.hdr.id = id;

   param.value.num += 20;
   if(param.value.num < min || param.value.num > max)
      param.value.num = min;
   result = mmal_port_parameter_set(camera->control, &param.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to set %s, %d", param_name, result);
      return MMAL_FALSE;
   }
   result = mmal_port_parameter_get(camera->control, &param_check.hdr);
   if (result != MMAL_SUCCESS)
   {
      LOG_ERROR("Failed to retrieve %s, %d", param_name, result);
      return MMAL_FALSE;
   }
   if (memcmp(&param, &param_check, sizeof(param)) != 0)
   {
      LOG_ERROR("%s set (%d/%d) was not retrieved (%d/%d)", param_name, 
                  param.value.num, param.value.den, 
                  param_check.value.num, param_check.value.den);
      return MMAL_FALSE;
   }
   return MMAL_TRUE;
}

/*****************************************************************************/
static MMAL_COMPONENT_T *test_video_encoder_create(MMALCAM_BEHAVIOUR_T *behaviour, MMAL_STATUS_T *status)
{
   MMAL_COMPONENT_T *encoder = 0;
   MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
   const char *component_name = MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER;
   uint32_t encoding;

   /* Set the port format */
   if (parse_vformat(behaviour->vformat, 0, 0, &encoding))
   {
      *status = MMAL_EINVAL;
      goto error;
   }

   if (encoding == MMAL_ENCODING_JPEG)
      component_name = MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER;

   *status = mmal_component_create(component_name, &encoder);
   if(*status != MMAL_SUCCESS)
   {
      LOG_ERROR("couldn't create video encoder");
      goto error;
   }
   if(!encoder->input_num || !encoder->output_num)
   {
      LOG_ERROR("video encoder doesn't have input/output ports");
      *status = MMAL_EINVAL;
      goto error;
   }

   encoder_input = encoder->input[0];
   encoder_output = encoder->output[0];

   mmal_format_copy(encoder_output->format, encoder_input->format);
   encoder_output->format->encoding = encoding;
   encoder_output->format->bitrate = behaviour->bit_rate;

   *status = mmal_port_format_commit(encoder_output);
   if(*status != MMAL_SUCCESS)
   {
      LOG_ERROR("format not set on video encoder output port");
      goto error;
   }
   encoder_output->buffer_size = encoder_output->buffer_size_recommended;
   if (encoder_output->buffer_size < encoder_output->buffer_size_min)
      encoder_output->buffer_size = encoder_output->buffer_size_min;
   encoder_output->buffer_num = encoder_output->buffer_num_recommended;
   if (encoder_output->buffer_num < encoder_output->buffer_num_min)
      encoder_output->buffer_num = encoder_output->buffer_num_min;

   if (enable_zero_copy())
   {
      MMAL_PARAMETER_BOOLEAN_T param_zc =
         {{MMAL_PARAMETER_ZERO_COPY, sizeof(MMAL_PARAMETER_BOOLEAN_T)}, 1};
      *status = mmal_port_parameter_set(encoder_output, &param_zc.hdr);
      if (*status != MMAL_SUCCESS && *status != MMAL_ENOSYS)
      {
         LOG_ERROR("failed to set zero copy on encoder output");
         goto error;
      }
      *status = mmal_port_parameter_set(encoder_input, &param_zc.hdr);
      if (*status != MMAL_SUCCESS && *status != MMAL_ENOSYS)
      {
         LOG_ERROR("failed to set zero copy on encoder input");
         goto error;
      }
      LOG_INFO("enabled zero copy on encoder");
   }

   if (behaviour->opaque)
   {
      encoder_input->format->encoding = MMAL_ENCODING_OPAQUE;
   }

    // set INLINE HEADER flag to generate SPS and PPS for every IDR if requested
    if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, ENABLE_MMAL_INLINE_HEADER) != MMAL_SUCCESS)
    {
        vcos_log_error("failed to set INLINE HEADER FLAG parameters");
        // Continue rather than abort..
    }

    //set flag for add SPS TIMING
    if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING, ENABLE_SPS_TIMING) != MMAL_SUCCESS)
    {
        vcos_log_error("failed to set SPS TIMINGS FLAG parameters");
        // Continue rather than abort..
    }

    if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_SEPARATE_NAL_BUFS, 1) != MMAL_SUCCESS)
    {
        vcos_log_error("failed to set SEPARATE_NAL_BUFS");
        // Continue rather than abort..
    }

{    
    MMAL_PARAMETER_UINT32_T param = {
        {MMAL_PARAMETER_INTRAPERIOD, sizeof(param)}, INTRAPERIOD};
    *status = mmal_port_parameter_set(encoder_output, &param.hdr);
    if (*status != MMAL_SUCCESS) {
        vcos_log_error("Unable to set intraperiod");
        goto error;
    }}

    
{    
    MMAL_PARAMETER_UINT32_T param = {
        {MMAL_PARAMETER_VIDEO_ENCODE_INITIAL_QUANT, sizeof(param)},
        QUANTISATION_PARAMETER};
    *status = mmal_port_parameter_set(encoder_output, &param.hdr);
    if (*status != MMAL_SUCCESS) {
        vcos_log_error("Unable to set initial QP");
        goto error;
    }
    
    MMAL_PARAMETER_UINT32_T param2 = {
        {MMAL_PARAMETER_VIDEO_ENCODE_MIN_QUANT, sizeof(param)},
        QUANTISATION_PARAMETER};
    *status = mmal_port_parameter_set(encoder_output, &param2.hdr);
    if (*status != MMAL_SUCCESS) {
        vcos_log_error("Unable to set min QP");
        goto error;
    }

    MMAL_PARAMETER_UINT32_T param3 = {
        {MMAL_PARAMETER_VIDEO_ENCODE_MAX_QUANT, sizeof(param)},
        QUANTISATION_PARAMETER};
    *status = mmal_port_parameter_set(encoder_output, &param3.hdr);
    if (*status != MMAL_SUCCESS) {
        vcos_log_error("Unable to set max QP");
        goto error;
    }}

{    
    MMAL_PARAMETER_VIDEO_PROFILE_T  param;
    param.hdr.id = MMAL_PARAMETER_PROFILE;
    param.hdr.size = sizeof(param);

    param.profile[0].profile = MMAL_VIDEO_PROFILE_H264_HIGH;

    param.profile[0].level = MMAL_VIDEO_LEVEL_H264_42;

    *status = mmal_port_parameter_set(encoder_output, &param.hdr);
    if (*status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to set H264 profile");
        goto error;
    }}

    //set INLINE VECTORS flag to request motion vector estimates
    if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS, 0) != MMAL_SUCCESS)
    {
        vcos_log_error("failed to set INLINE VECTORS parameters");
        // Continue rather than abort..
    }

   if (mmal_port_parameter_set_boolean(encoder_input, MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, IMMUTABLE_OUTPUT) != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to set immutable input flag");
      // Continue rather than abort..
   }

   /* Enable component */
   *status = mmal_component_enable(encoder);
   if(*status)
   {
      LOG_ERROR("video encoder component couldn't be enabled");
      goto error;
   }

   return encoder;

 error:
   if(encoder) mmal_component_destroy(encoder);
   return 0;
}

/*****************************************************************************/
MMAL_PORT_T *encoder_input = 0, *encoder_output = 0;
void request_i_frame() {
    if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_REQUEST_I_FRAME, 1) != MMAL_SUCCESS)
    {
        vcos_log_error("failed to request I-FRAME");
    }
}

int mmal_start_camcorder(volatile int *stop, MMALCAM_BEHAVIOUR_T *behaviour, on_buffer_cb cb)
{
   MMAL_STATUS_T status = MMAL_SUCCESS;
   MMAL_POOL_T *pool_encoder_in = 0, *pool_encoder_out = 0;
   MMAL_QUEUE_T *queue_encoder_in = 0, *queue_encoder_out = 0;
   MMAL_COMPONENT_T *camera = 0, *encoder = 0;
   MMAL_PORT_T *video_port = 0;
   uint32_t ms_per_change, last_change_ms, set_focus_delay_ms;
   int packet_count = 0;

   if(vcos_event_flags_create(&events, "MMALCam") != VCOS_SUCCESS)
   {
      behaviour->init_result = MMALCAM_INIT_ERROR_EVENT_FLAGS;
      goto error;
   }

   zero_copy = behaviour->zero_copy;
   tunneling = behaviour->tunneling;

   /* Create and setup camera viewfinder component */
   camera = test_camera_create(behaviour, &status);
   if(!camera)
   {
      behaviour->init_result = MMALCAM_INIT_ERROR_CAMERA;
      goto error;
   }
   video_port = camera->output[1];

   /*...*/
   MMAL_PARAMETER_BOOLEAN_T camera_capture =
         {{MMAL_PARAMETER_CAPTURE, sizeof(MMAL_PARAMETER_BOOLEAN_T)}, 1};

   /* Create and setup video encoder component */
   encoder = test_video_encoder_create(behaviour, &status);
   if(!encoder)
   {
      behaviour->init_result = MMALCAM_INIT_ERROR_ENCODER;
      goto error;
   }
   encoder_input = encoder->input[0];
   encoder_output = encoder->output[0];

   status = connect_ports(video_port, encoder_input, &queue_encoder_in, &pool_encoder_in);
   if (status != MMAL_SUCCESS)
   {
      behaviour->init_result = MMALCAM_INIT_ERROR_ENCODER_IN;
      goto error;
   }

   status = setup_output_port(encoder_output, &queue_encoder_out, &pool_encoder_out);
   if (status != MMAL_SUCCESS)
   {
      behaviour->init_result = MMALCAM_INIT_ERROR_ENCODER_OUT;
      goto error;
   }

   status = mmal_port_parameter_set(video_port, &camera_capture.hdr);
   if (status != MMAL_SUCCESS && status != MMAL_ENOSYS)
   {
      behaviour->init_result = MMALCAM_INIT_ERROR_CAMERA_CAPTURE;
      goto error;
   }

   /* Initialisation now complete */
   behaviour->init_result = MMALCAM_INIT_SUCCESS;
   vcos_semaphore_post(&behaviour->init_sem);

   ms_per_change = behaviour->seconds_per_change * 1000;
   last_change_ms = vcos_get_ms();
   set_focus_delay_ms = 1000;
    int is_start = 0;
   while(1)
   {
      MMAL_BUFFER_HEADER_T *buffer;
      VCOS_UNSIGNED set;

      vcos_event_flags_get(&events, MMAL_CAM_ANY_EVENT, VCOS_OR_CONSUME, VCOS_TICKS_TO_MS(2), &set);
      if(*stop) break;

      if (behaviour->focus_test != MMAL_PARAM_FOCUS_MAX)
      {
         if (set & MMAL_CAM_AUTOFOCUS_COMPLETE ||
               (set_focus_delay_ms && (vcos_get_ms() - last_change_ms) >= set_focus_delay_ms))
         {
            set_focus_delay_ms = 0;
            mmalcam_reset_focus(camera, behaviour->focus_test);
         }
      }

      status = fill_port_from_pool(video_port, pool_encoder_in);
      if (status != MMAL_SUCCESS)
         break;
      status = fill_port_from_pool(encoder_output, pool_encoder_out);
      if (status != MMAL_SUCCESS)
         break;

      status = send_buffer_from_queue(encoder_input, queue_encoder_in);
      if (status != MMAL_SUCCESS)
         break;

      /* Process output buffers from encoder */
      if (queue_encoder_out)
      {
         buffer = mmal_queue_get(queue_encoder_out);
         if (buffer)
         {
            mmal_buffer_header_mem_lock(buffer);
            
            cb(buffer);

            mmal_buffer_header_mem_unlock(buffer);
            packet_count++;
            mmal_buffer_header_release(buffer);
         }
      }

      /* Change a camera parameter if requested */
      if (ms_per_change != 0)
      {
         if((vcos_get_ms() - last_change_ms) >= ms_per_change)
         {
            last_change_ms = vcos_get_ms();
            switch (behaviour->change)
            {
               case MMALCAM_CHANGE_IMAGE_EFFECT:
                  if (!mmalcam_next_effect(camera))
                     *stop = 1;
                  break;
               case MMALCAM_CHANGE_ROTATION:
                  if (!mmalcam_next_rotation(camera))
                     *stop = 1;
                  break;
               case MMALCAM_CHANGE_ZOOM:
                  if (!mmalcam_next_zoom(camera))
                     *stop = 1;
                  break;
               case MMALCAM_CHANGE_FOCUS:
                  if (!mmalcam_next_focus(camera))
                     *stop = 1;
                  break;
               case MMALCAM_CHANGE_DRC:
                  if (!mmalcam_next_drc(camera))
                     *stop = 1;
                  break;
               case MMALCAM_CHANGE_HDR:
                  if (!mmalcam_next_hdr(camera))
                     *stop = 1;
                  break;
               case MMALCAM_CHANGE_CONTRAST:
                  if (!mmalcam_next_colour_param(camera, MMAL_PARAMETER_CONTRAST, -100, 100, "contrast"))
                     *stop = 1;
                  break;
               case MMALCAM_CHANGE_BRIGHTNESS:
                  if (!mmalcam_next_colour_param(camera, MMAL_PARAMETER_BRIGHTNESS, 0, 100, "brightness"))
                     *stop = 1;
                  break;
               case MMALCAM_CHANGE_SATURATION:
                  if (!mmalcam_next_colour_param(camera, MMAL_PARAMETER_SATURATION, -100, 100, "saturation"))
                     *stop = 1;
                  break;
               case MMALCAM_CHANGE_SHARPNESS:
                  if (!mmalcam_next_colour_param(camera, MMAL_PARAMETER_SHARPNESS, -100, 100, "sharpness"))
                     *stop = 1;
                  break;
               default:
                  LOG_ERROR("Unexpected change behaviour: %d", behaviour->change);
                  break;
            }
         }
      }
   }

   /* Disable ports */
   disable_port(video_port);
   disable_port(encoder_input);
   disable_port(encoder_output);

   /* Disable components */
   if (encoder)
      mmal_component_disable(encoder);
   mmal_component_disable(camera);

   INIT_PARAMETER(behaviour->render_stats, MMAL_PARAMETER_STATISTICS);
//    mmal_port_parameter_get(render_port, &behaviour->render_stats.hdr);
   if (encoder)
   {
      INIT_PARAMETER(behaviour->encoder_stats, MMAL_PARAMETER_STATISTICS);
      mmal_port_parameter_get(encoder_output, &behaviour->encoder_stats.hdr);
   }

 error:
   /* The pools need to be destroyed first since they are owned by the components */
   if(pool_encoder_in)
      mmal_port_pool_destroy(video_port, pool_encoder_in);
   if(pool_encoder_out)
      mmal_port_pool_destroy(encoder_output, pool_encoder_out);

   if(encoder)
      mmal_component_destroy(encoder);
   if(camera)
      mmal_component_destroy(camera);

   if(queue_encoder_in)
      mmal_queue_destroy(queue_encoder_in);
   if(queue_encoder_out)
      mmal_queue_destroy(queue_encoder_out);

   vcos_event_flags_delete(&events);

   if (packet_count)
      printf("Packet count: %d\n", packet_count);

   if (behaviour->init_result != MMALCAM_INIT_SUCCESS)
      vcos_semaphore_post(&behaviour->init_sem);

   return (int)status;
}
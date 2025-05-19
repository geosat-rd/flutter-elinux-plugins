// Copyright 2021 Sony Group Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Alyson 2025-01-21_004

#include "gst_video_player.h"

#include <iostream>

#define MAX_WIDTH 8192
#define MAX_HEIGHT 8192

GstVideoPlayer::GstVideoPlayer(
    const std::string &uri, std::unique_ptr<VideoPlayerStreamHandler> handler)
    : stream_handler_(std::move(handler))
{
    std::cerr << "test GstVideoPlayer" << std::endl;

    gst_.pipeline = nullptr;
    gst_.playbin = nullptr;
    gst_.video_convert = nullptr;
    gst_.video_sink = nullptr;
    gst_.output = nullptr;
    gst_.bus = nullptr;
    gst_.buffer = nullptr;
    gst_.source = nullptr;
    gst_.depay = nullptr;
    gst_.parse = nullptr;
    gst_.decoder = nullptr;

    uri_ = ParseUri(uri);
    if (!CreatePipeline())
    {
	std::cerr << "Failed to create a pipeline" << std::endl;
	DestroyPipeline();
	return;
    }
}

GstVideoPlayer::~GstVideoPlayer()
{
#ifdef USE_EGL_IMAGE_DMABUF
    UnrefEGLImage();
#endif // USE_EGL_IMAGE_DMABUF
    Stop();
    DestroyPipeline();
}

// static
void GstVideoPlayer::GstLibraryLoad()
{
    gst_init(NULL, NULL);
}

// static
void GstVideoPlayer::GstLibraryUnload()
{
    gst_deinit();
}

bool GstVideoPlayer::Init()
{
    if (!gst_.pipeline)
    {
	return false;
    }

    // Prerolls before getting information from the pipeline.
    if (!Preroll())
    {
	DestroyPipeline();
	return false;
    }

    // Sets internal video size and buffier.
    GetVideoSize(width_, height_);
    pixels_.reset(new uint32_t[width_ * height_]);

    stream_handler_->OnNotifyInitialized();

    return true;
}

bool GstVideoPlayer::Play()
{
    if (gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING) ==
	GST_STATE_CHANGE_FAILURE)
    {
	std::cerr << "Failed to change the state to PLAYING" << std::endl;
	return false;
    }

    std::cerr << "GstVideoPlayer::Play" << std::endl;
    stream_handler_->OnNotifyPlaying(true);

    return true;
}

bool GstVideoPlayer::Pause()
{
    if (gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED) ==
	GST_STATE_CHANGE_FAILURE)
    {
	std::cerr << "Failed to change the state to PAUSED" << std::endl;
	return false;
    }

    stream_handler_->OnNotifyPlaying(false);
    return true;
}

bool GstVideoPlayer::Stop()
{
    if (gst_element_set_state(gst_.pipeline, GST_STATE_READY) ==
	GST_STATE_CHANGE_FAILURE)
    {
	std::cerr << "Failed to change the state to READY" << std::endl;
	return false;
    }

    stream_handler_->OnNotifyPlaying(false);
    return true;
}

bool GstVideoPlayer::SetVolume(double volume)
{
    if (!gst_.pipeline)
    {
	return false;
    }

    volume_ = volume;
    g_object_set(gst_.pipeline, "volume", volume, NULL);
    return true;
}

bool GstVideoPlayer::SetPlaybackRate(double rate)
{
    if (!gst_.pipeline)
    {
	return false;
    }

    if (rate <= 0)
    {
	std::cerr << "Rate " << rate << " is not supported" << std::endl;
	return false;
    }

    auto position = GetCurrentPosition();
    if (position < 0)
    {
	return false;
    }

    if (!gst_element_seek(gst_.pipeline, rate, GST_FORMAT_TIME,
			  GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET,
			  position * GST_MSECOND, GST_SEEK_TYPE_SET,
			  GST_CLOCK_TIME_NONE))
    {
	std::cerr << "Failed to set playback rate to " << rate
		  << " (gst_element_seek failed)" << std::endl;
	return false;
    }

    playback_rate_ = rate;
    mute_ = (rate < 0.5 || rate > 2);
    g_object_set(gst_.pipeline, "mute", mute_, NULL);

    return true;
}

bool GstVideoPlayer::SetSeek(int64_t position)
{
    auto nanosecond = position * 1000 * 1000;
    if (!gst_element_seek(
	    gst_.pipeline, playback_rate_, GST_FORMAT_TIME,
	    (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
	    GST_SEEK_TYPE_SET, nanosecond, GST_SEEK_TYPE_SET,
	    GST_CLOCK_TIME_NONE))
    {
	std::cerr << "Failed to seek " << nanosecond << std::endl;
	return false;
    }
    return true;
}

int64_t GstVideoPlayer::GetDuration()
{
    GstFormat fmt = GST_FORMAT_TIME;
    gint64 duration_msec;
    if (!gst_element_query_duration(gst_.pipeline, fmt, &duration_msec))
    {
	std::cerr << "Failed to get duration" << std::endl;
	return -1;
    }
    duration_msec /= GST_MSECOND;
    return duration_msec;
}

int64_t GstVideoPlayer::GetCurrentPosition()
{
    gint64 position = 0;

    // Sometimes we get an error when playing streaming videos.
    if (!gst_element_query_position(gst_.pipeline, GST_FORMAT_TIME, &position))
    {
	std::cerr << "Failed to get current position" << std::endl;
	return -1;
    }

    // TODO: We need to handle this code in the proper plase.
    // The VideoPlayer plugin doesn't have a main loop, so EOS message
    // received from GStreamer cannot be processed directly in a callback
    // function. This is because the event channel message of playback complettion
    // needs to be thrown in the main thread.
    {
	std::unique_lock<std::mutex> lock(mutex_event_completed_);
	if (is_completed_)
	{
	    is_completed_ = false;
	    lock.unlock();
	    if (auto_repeat_)
	    {
		SetSeek(0);
	    }
	    else
	    {
		stream_handler_->OnNotifyCompleted();
	    }
	}
    }

    return position / GST_MSECOND;
}

#ifdef USE_EGL_IMAGE_DMABUF
void *GstVideoPlayer::GetEGLImage(void *egl_display, void *egl_context)
{
    std::shared_lock<std::shared_mutex> lock(mutex_buffer_);
    if (!gst_.buffer)
    {
	return nullptr;
    }

    GstMemory *memory = gst_buffer_peek_memory(gst_.buffer, 0);
    if (gst_is_dmabuf_memory(memory))
    {
	UnrefEGLImage();

	gint fd = gst_dmabuf_memory_get_fd(memory);
	gst_gl_display_egl_ =
	    gst_gl_display_egl_new_with_egl_display(reinterpret_cast<gpointer>(egl_display));
	gst_gl_ctx_ = gst_gl_context_new_wrapped(
	    GST_GL_DISPLAY_CAST(gst_gl_display_egl_), reinterpret_cast<guintptr>(egl_context),
	    GST_GL_PLATFORM_EGL, GST_GL_API_GLES2);

	gst_gl_context_activate(gst_gl_ctx_, TRUE);

	gst_egl_image_ =
	    gst_egl_image_from_dmabuf(gst_gl_ctx_, fd, &gst_video_info_, 0, 0);
	return reinterpret_cast<void *>(gst_egl_image_get_image(gst_egl_image_));
    }
    return nullptr;
}

void GstVideoPlayer::UnrefEGLImage()
{
    if (gst_egl_image_)
    {
	gst_egl_image_unref(gst_egl_image_);
	gst_object_unref(gst_gl_ctx_);
	gst_object_unref(gst_gl_display_egl_);
	gst_egl_image_ = NULL;
	gst_gl_ctx_ = NULL;
	gst_gl_display_egl_ = NULL;
    }
}
#endif // USE_EGL_IMAGE_DMABUF

const uint8_t *GstVideoPlayer::GetFrameBuffer()
{
    std::shared_lock<std::shared_mutex> lock(mutex_buffer_);
    if (!gst_.buffer)
    {
	return nullptr;
    }

    const uint32_t pixel_bytes = width_ * height_ * 4;
  //  const uint32_t pixel_bytes = width_ * height_ * 1.5; => for NV12/I412

    gst_buffer_extract(gst_.buffer, 0, pixels_.get(), pixel_bytes);

    return reinterpret_cast<const uint8_t *>(pixels_.get());
}

// Creats a video pipeline using playbin.
// $ playbin uri=<file> video-sink="videoconvert ! video/x-raw,format=RGBA !
// fakesink"
bool GstVideoPlayer::CreatePipeline()
{

    // [1/10] Create the pipeline
    gst_.pipeline = gst_pipeline_new("pipeline");
    if (!gst_.pipeline)
    {
	return false;
    }

    // [2/10] Create GStreamer elements
    gst_.source = gst_element_factory_make("rtspsrc", "source");
    gst_.depay = gst_element_factory_make("rtph264depay", "depay");
    gst_.parse = gst_element_factory_make("h264parse", "parse");
    gst_.decoder = gst_element_factory_make("qtivdec", "decoder");
    gst_.video_convert = gst_element_factory_make("videoconvert", "videoconvert");
    gst_.video_sink = gst_element_factory_make("fakesink", "videosink"); // Fake sink to handle video frames
    gst_.queue = gst_element_factory_make("queue", "queue");

    // [3/10] Check if elements are created successfully
    if (!gst_.source || !gst_.depay || !gst_.parse || !gst_.decoder ||
	!gst_.video_convert || !gst_.video_sink)
    {
	return false;
    }

    g_object_set(G_OBJECT(gst_.queue),
		 "max-size-buffers", 1, // Limit to 1 buffers to keep latency low
		 "max-size-bytes", 0,   // No byte limit
		 //"max-size-time", 0,    // No time limit
		 "max-size-time", GST_MSECOND * 5,    // Limit handle queue for 20ms
		 "leaky", 2,            // Leak downstream (drop old frames) if full
		 NULL);

    /*
     * For leaky:
     * Value                Behavior
     *  0 (No Leak)         Blocks upstream until space is free
     *  1 (Upstream Leak)   Drops new incoming buffers
     *  2 (Downstream Leak) Drops old buffers in the queue
     */

    // [4/10] Set properties for RTSP source
    g_object_set(G_OBJECT(gst_.source),
		 "location", uri_.c_str(),   // RTSP stream URI
		 "latency", 0,               // Buffer latency in ms
		 "buffer-mode", 0,           // Enable low latency mode
		 "do-retransmission", FALSE, // Disable packet retransmission
		 "protocols", 0x00000004,    // Use TCP for transport
		 "drop-on-latency", TRUE,    // Drop frames if latency exceeds threshold
		 NULL);

    // [5/10] Set properties for fakesink
    g_object_set(G_OBJECT(gst_.video_sink),
		 "sync", FALSE,           // Disable sync to reduce latency
		 "async", FALSE,          // Disable async mode for immediate processing
		 "signal-handoffs", TRUE, // Enable handoff signal to get frames
		 NULL);

    // [6/10] Add all elements to the pipeline
    gst_bin_add_many(GST_BIN(gst_.pipeline),
		     gst_.source, gst_.depay, gst_.parse, gst_.decoder,
		     gst_.video_convert, gst_.queue, gst_.video_sink, NULL);

    // [7/10] Link static elements
    if (!gst_element_link_many(gst_.depay, gst_.parse, gst_.decoder, gst_.video_convert, gst_.queue, NULL))
    {
	return false;
    }

    // Link `videoconvert` to `fakesink` with a caps filter for RGBA format
    auto *caps = gst_caps_from_string("video/x-raw,format=RGBA");
  //  auto *caps = gst_caps_from_string("video/x-raw,format=NV12"); -> X
  //  auto *caps = gst_caps_from_string("video/x-raw,format=I412"); -> Failed to link
    if (!gst_element_link_filtered(gst_.queue, gst_.video_sink, caps))
    {
	gst_caps_unref(caps);
	return false;
    }
    gst_caps_unref(caps);

    // [8/10] Connect dynamic pad-added signal for RTSP source
    g_signal_connect(gst_.source, "pad-added", G_CALLBACK(onPadAdded), gst_.depay);

    // [9/10] Connect handoff signal for fakesink to process frames
    g_signal_connect(gst_.video_sink, "handoff", G_CALLBACK(HandoffHandler), this);

    // [10/10] Set up pipeline bus and sync handler
    gst_.bus = gst_pipeline_get_bus(GST_PIPELINE(gst_.pipeline));
    if (!gst_.bus)
    {
	return false;
    }
    gst_bus_set_sync_handler(gst_.bus, HandleGstMessage, this, NULL);

    return true;
}

bool GstVideoPlayer::Preroll()
{
    if (!gst_.pipeline)
    {
	return false;
    }

    auto result = gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED);
    if (result == GST_STATE_CHANGE_FAILURE)
    {
	return false;
    }

    if (result == GST_STATE_CHANGE_ASYNC)
    {
	GstState state, pending;
	result = gst_element_get_state(gst_.pipeline, &state, &pending, GST_CLOCK_TIME_NONE);
	if (result == GST_STATE_CHANGE_FAILURE)
	{
	    return false;
	}
    }

    return true;
}

void GstVideoPlayer::DestroyPipeline()
{

    // Disable the "handoff" signal to prevent further callbacks
    if (gst_.video_sink)
    {
	g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", FALSE, NULL);
    }

    // Set the pipeline state to NULL to release all resources
    if (gst_.pipeline)
    {
	gst_element_set_state(gst_.pipeline, GST_STATE_NULL);
    }

    // Unreference the buffer if it exists
    if (gst_.buffer)
    {
	gst_buffer_unref(gst_.buffer);
	gst_.buffer = nullptr;
    }

    // Unreference and clear the bus
    if (gst_.bus)
    {
	gst_object_unref(gst_.bus);
	gst_.bus = nullptr;
    }

    // Unreference and clear the pipeline
    if (gst_.pipeline)
    {
	gst_object_unref(gst_.pipeline);
	gst_.pipeline = nullptr;
    }

    // Unreference and clear the output
    if (gst_.output)
    {
	gst_object_unref(gst_.output);
	gst_.output = nullptr;
    }

    // Unreference and clear the video sink
    if (gst_.video_sink)
    {
	gst_object_unref(gst_.video_sink);
	gst_.video_sink = nullptr;
    }

    // Unreference and clear the video convert
    if (gst_.video_convert)
    {
	gst_object_unref(gst_.video_convert);
	gst_.video_convert = nullptr;
    }

    // Unreference and clear the decoder
    if (gst_.decoder)
    {
	gst_object_unref(gst_.decoder);
	gst_.decoder = nullptr;
    }

    // Unreference and clear the parse
    if (gst_.parse)
    {
	gst_object_unref(gst_.parse);
	gst_.parse = nullptr;
    }

    // Unreference and clear the depay
    if (gst_.depay)
    {
	gst_object_unref(gst_.depay);
	gst_.depay = nullptr;
    }

    // Unreference and clear the source
    if (gst_.source)
    {
	gst_object_unref(gst_.source);
	gst_.source = nullptr;
    }

}

std::string GstVideoPlayer::ParseUri(const std::string &uri)
{
    if (gst_uri_is_valid(uri.c_str()))
    {
	return uri;
    }

    auto *filename_uri = gst_filename_to_uri(uri.c_str(), NULL);
    if (!filename_uri)
    {
	std::cerr << "Faild to open " << uri.c_str() << std::endl;
	return uri;
    }
    std::string result_uri(filename_uri);
    g_free(filename_uri);

    return result_uri;
}

void GstVideoPlayer::GetVideoSize(int32_t &width, int32_t &height)
{

    if (width <= 0 || height <= 0 || width > MAX_WIDTH || height > MAX_HEIGHT)
    {
	width = 0;
	height = 0;
	return;
    }

    if (!gst_.pipeline || !gst_.video_sink)
    {
	std::cerr
	    << "Failed to get video size. The pileline hasn't initialized yet.";
	return;
    }

    auto *sink_pad = gst_element_get_static_pad(gst_.video_sink, "sink");
    if (!sink_pad)
    {
	std::cerr << "Failed to get a pad";
	return;
    }

    auto *caps = gst_pad_get_current_caps(sink_pad);
    if (!caps || gst_caps_get_size(caps) == 0)
    {
	std::cerr << "Failed to get caps" << std::endl;
	gst_object_unref(sink_pad);
	return;
    }

    auto *structure = gst_caps_get_structure(caps, 0);
    if (!structure)
    {
	std::cerr << "Failed to get a structure" << std::endl;
	gst_caps_unref(caps);
	gst_object_unref(sink_pad);
	return;
    }

    gst_structure_get_int(structure, "width", &width);
    gst_structure_get_int(structure, "height", &height);

#ifdef USE_EGL_IMAGE_DMABUF
    gboolean res = gst_video_info_from_caps(&gst_video_info_, caps);
    if (!res)
    {
	std::cerr << "Failed to get a gst_video_info" << std::endl;
	gst_caps_unref(caps);
	gst_object_unref(sink_pad);
	return;
    }
#endif // USE_EGL_IMAGE_DMABUF

    gst_caps_unref(caps);
    gst_object_unref(sink_pad);
}

// static
void GstVideoPlayer::onPadAdded(GstElement *src, GstPad *new_pad, GstElement *depay)
{
    std::cout << "test onPadAdded" << std::endl;
    GstPad *sink_pad = gst_element_get_static_pad(depay, "sink");
    if (!gst_pad_is_linked(sink_pad))
    {
	GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
	if (ret != GST_PAD_LINK_OK)
	{
	    std::cerr << "Failed to link dynamic pad from source to depayloader" << std::endl;
	}
    }
    gst_object_unref(sink_pad);
}

// static
void GstVideoPlayer::HandoffHandler(GstElement *fakesink, GstBuffer *buf, GstPad *new_pad, gpointer user_data)
{

    auto *self = reinterpret_cast<GstVideoPlayer *>(user_data);

    auto *caps = gst_pad_get_current_caps(new_pad);
    if (!caps || gst_caps_get_size(caps) == 0)
    {
	std::cerr << "[Handoff] Failed to get valid caps" << std::endl;
	return;
    }

    auto *structure = gst_caps_get_structure(caps, 0);
    if (!structure)
    {
	std::cerr << "[Handoff] Caps has no structure" << std::endl;
	gst_caps_unref(caps);
	return;
    }

    int width = 0, height = 0;
    if (!gst_structure_get_int(structure, "width", &width) ||
	!gst_structure_get_int(structure, "height", &height))
    {
	gst_caps_unref(caps);
	return;
    }
    gst_caps_unref(caps);

    if (width != self->width_ || height != self->height_)
    {
	self->width_ = width;
	self->height_ = height;
	try
	{
	    self->pixels_.reset(new uint32_t[width * height]);
	}
	catch (const std::bad_alloc &e)
	{
	    return;
	}
    }

    {
	std::lock_guard<std::shared_mutex> lock(self->mutex_buffer_);
	if (self->gst_.buffer)
	{
	    gst_buffer_unref(self->gst_.buffer);
	}
	self->gst_.buffer = gst_buffer_ref(buf);
    }

    self->stream_handler_->OnNotifyFrameDecoded();

}

// static
GstBusSyncReply GstVideoPlayer::HandleGstMessage(GstBus *bus,
						 GstMessage *message,
						 gpointer user_data)
{
    switch (GST_MESSAGE_TYPE(message))
    {
    case GST_MESSAGE_EOS:
    {
	auto *self = reinterpret_cast<GstVideoPlayer *>(user_data);
	std::lock_guard<std::mutex> lock(self->mutex_event_completed_);
	self->is_completed_ = true;
	break;
    }
    case GST_MESSAGE_WARNING:
    {
	gchar *debug;
	GError *error;
	gst_message_parse_warning(message, &error, &debug);
	g_printerr("WARNING from element %s: %s\n", GST_OBJECT_NAME(message->src),
		   error->message);
	g_printerr("Warning details: %s\n", debug);
	g_free(debug);
	g_error_free(error);
	break;
    }
    case GST_MESSAGE_ERROR:
    {
	gchar *debug;
	GError *error;
	gst_message_parse_error(message, &error, &debug);
	g_printerr("ERROR from element %s: %s\n", GST_OBJECT_NAME(message->src),
		   error->message);
	g_printerr("Error details: %s\n", debug);
	g_free(debug);
	g_error_free(error);
	break;
    }
    default:
	break;
    }

    gst_message_unref(message);

    return GST_BUS_DROP;
}

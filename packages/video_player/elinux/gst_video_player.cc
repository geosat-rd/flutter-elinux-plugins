// Copyright 2021 Sony Group Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gst_video_player.h"

#include <iostream>

#define MAX_WIDTH 8192
#define MAX_HEIGHT 8192

// 確保所有成員變數都有合理初值（否則會出現「rtsp width 1919903860」那種亂數）
GstVideoPlayer::GstVideoPlayer(
    const std::string& uri, std::unique_ptr<VideoPlayerStreamHandler> handler)
    : stream_handler_(std::move(handler)) {
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
    gst_.decodebin = nullptr;

  uri_ = ParseUri(uri);
  is_rtsp_ = (uri_.find("rtsp://") == 0);
  if (!CreatePipeline()) {
    std::cerr << "Failed to create a pipeline" << std::endl;
    DestroyPipeline();
    return;
  }
}

GstVideoPlayer::~GstVideoPlayer() {
#ifdef USE_EGL_IMAGE_DMABUF
  UnrefEGLImage();
#endif  // USE_EGL_IMAGE_DMABUF
  Stop();
  DestroyPipeline();
}

// static
void GstVideoPlayer::GstLibraryLoad() { gst_init(NULL, NULL); }

// static
void GstVideoPlayer::GstLibraryUnload() { gst_deinit(); }

bool GstVideoPlayer::Init() {
  if (!gst_.pipeline) {
    return false;
  }

  // Prerolls before getting information from the pipeline.
  if (!Preroll()) {
    DestroyPipeline();
    return false;
  }

  // Sets internal video size and buffier.
  GetVideoSize(width_, height_);
  pixels_.reset(new uint32_t[width_ * height_]);

  // stream_handler_->OnNotifyInitialized();

  return true;
}

bool GstVideoPlayer::Play() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PLAYING" << std::endl;
    return false;
  }

  stream_handler_->OnNotifyPlaying(true);
  is_playing_ = true;
  return true;
}

bool GstVideoPlayer::Pause() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PAUSED" << std::endl;
    return false;
  }

  stream_handler_->OnNotifyPlaying(false);
  is_playing_ = false;
  return true;
}

bool GstVideoPlayer::Stop() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_READY) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to READY" << std::endl;
    return false;
  }

  stream_handler_->OnNotifyPlaying(false);
  is_playing_ = false;
  return true;
}

bool GstVideoPlayer::SetVolume(double volume) {
  volume_ = volume;
  if (!is_rtsp_) {
    if (!gst_.playbin) return false;
    g_object_set(gst_.playbin, "volume", volume, NULL);
  } else {
    // RTSP pipeline 沒有 playbin 屬性，若要 mute/volume 可透過 audio elements 或忽略
    // std::cerr << "SetVolume: RTSP pipeline - volume not supported on pipeline object" << std::endl;
  }
  return true;
}

bool GstVideoPlayer::SetPlaybackRate(double rate) {
  if (is_rtsp_) {
    return false;
  } else {
    if (!gst_.playbin) {
      return false;
    }
  }
  

  if (rate <= 0) {
    std::cerr << "Rate " << rate << " is not supported" << std::endl;
    return false;
  }

  auto position = GetCurrentPosition();
  if (position < 0) {
    return false;
  }

  if (!gst_element_seek(gst_.pipeline, rate, GST_FORMAT_TIME,
                        GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET,
                        position * GST_MSECOND, GST_SEEK_TYPE_SET,
                        GST_CLOCK_TIME_NONE)) {
    std::cerr << "Failed to set playback rate to " << rate
              << " (gst_element_seek failed)" << std::endl;
    return false;
  }

  playback_rate_ = rate;
  mute_ = (rate < 0.5 || rate > 2);
  
  if (is_rtsp_) {
    g_object_set(gst_.pipeline, "mute", mute_, NULL);
  } else {
    g_object_set(gst_.playbin, "mute", mute_, NULL);
  }
  
  return true;
}

bool GstVideoPlayer::SetSeek(int64_t position) {
  auto nanosecond = position * 1000 * 1000;
  if (!gst_element_seek(
          gst_.pipeline, playback_rate_, GST_FORMAT_TIME,
          (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
          GST_SEEK_TYPE_SET, nanosecond, GST_SEEK_TYPE_SET,
          GST_CLOCK_TIME_NONE)) {
    std::cerr << "Failed to seek " << nanosecond << std::endl;
    return false;
  }
  
  if (!is_playing_)
  {
    gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING);
    gst_element_get_state(gst_.pipeline, nullptr, nullptr, 500 * GST_MSECOND);
    gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED);
    gst_element_get_state(gst_.pipeline, nullptr, nullptr, 500 * GST_MSECOND);
  }
  return true;
}

int64_t GstVideoPlayer::GetDuration() {
  GstFormat fmt = GST_FORMAT_TIME;
  gint64 duration_msec;
  if (!gst_element_query_duration(gst_.pipeline, fmt, &duration_msec)) {
    //std::cerr << "Failed to get duration" << std::endl;
    return -1;
  }
  duration_msec /= GST_MSECOND;
  return duration_msec;
}

int64_t GstVideoPlayer::GetCurrentPosition() {
  gint64 position = 0;

  // Sometimes we get an error when playing streaming videos.
  if (!gst_element_query_position(gst_.pipeline, GST_FORMAT_TIME, &position)) {
    // std::cerr << "Failed to get current position" << std::endl;
    return -1;
  }

  // TODO: We need to handle this code in the proper plase.
  // The VideoPlayer plugin doesn't have a main loop, so EOS message
  // received from GStreamer cannot be processed directly in a callback
  // function. This is because the event channel message of playback complettion
  // needs to be thrown in the main thread.
  {
    std::unique_lock<std::mutex> lock(mutex_event_completed_);
    if (is_completed_) {
      is_completed_ = false;
      lock.unlock();

      if (auto_repeat_) {
        SetSeek(0);
      } else {
        stream_handler_->OnNotifyCompleted();
      }
    }
  }

  return position / GST_MSECOND;
}

#ifdef USE_EGL_IMAGE_DMABUF
void* GstVideoPlayer::GetEGLImage(void* egl_display, void* egl_context) {
  std::shared_lock<std::shared_mutex> lock(mutex_buffer_);
  if (!gst_.buffer) {
    return nullptr;
  }

  GstMemory* memory = gst_buffer_peek_memory(gst_.buffer, 0);
  if (gst_is_dmabuf_memory(memory)) {
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
    return reinterpret_cast<void*>(gst_egl_image_get_image(gst_egl_image_));
  }
  return nullptr;
}

void GstVideoPlayer::UnrefEGLImage() {
  if (gst_egl_image_) {
    gst_egl_image_unref(gst_egl_image_);
    gst_object_unref(gst_gl_ctx_);
    gst_object_unref(gst_gl_display_egl_);
    gst_egl_image_ = NULL;
    gst_gl_ctx_ = NULL;
    gst_gl_display_egl_ = NULL;
  }
}
#endif  // USE_EGL_IMAGE_DMABUF

const uint8_t* GstVideoPlayer::GetFrameBuffer() {
  std::shared_lock<std::shared_mutex> lock(mutex_buffer_);
  if (!gst_.buffer) {
    return nullptr;
  }

  const uint32_t pixel_bytes = width_ * height_ * 4;
  gst_buffer_extract(gst_.buffer, 0, pixels_.get(), pixel_bytes);
  return reinterpret_cast<const uint8_t*>(pixels_.get());
}

bool GstVideoPlayer::CreatePipeline() {
  std::cerr << "[DEBUG] uri_ = " << uri_ << std::endl;
  if (is_rtsp_) {
    std::cerr << "[DEBUG] Creating low-latency RTSP pipeline..." << std::endl;
    return CreateLowLatencyRTSPPipeline();
  } else {
    std::cerr << "[DEBUG] Creating auto-decode file pipeline..." << std::endl;
    return CreateAutoDecodeFilePipeline();
  }
}

bool GstVideoPlayer::CreateLowLatencyRTSPPipeline() {
    // [1/8] Create the pipeline
    gst_.pipeline = gst_pipeline_new("pipeline");
    if (!gst_.pipeline) {
        std::cerr << "Failed to create pipeline" << std::endl;
        return false;
    }

    // [2/8] Create elements
    gst_.source = gst_element_factory_make("rtspsrc", "source");
    gst_.decodebin = gst_element_factory_make("decodebin", "decodebin");
    gst_.video_convert = gst_element_factory_make("videoconvert", "videoconvert");
    gst_.queue = gst_element_factory_make("queue", "queue");
    gst_.video_sink = gst_element_factory_make("fakesink", "videosink");

    if (!gst_.source || !gst_.decodebin || !gst_.video_convert || !gst_.queue || !gst_.video_sink) {
        std::cerr << "Failed to create elements" << std::endl;
        return false;
    }

    // [3/8] Configure rtspsrc
    g_object_set(G_OBJECT(gst_.source),
                 "location", uri_.c_str(),
                 "latency", 0,
                 "buffer-mode", 0,
                 "do-retransmission", FALSE,
                 "protocols", 0x00000004, // GST_RTSP_LOWER_TRANS_UDP
                 "drop-on-latency", TRUE,
                 NULL);

    // [4/8] Configure fakesink
    g_object_set(G_OBJECT(gst_.video_sink),
                 "sync", FALSE,
                 "async", FALSE,
                 "signal-handoffs", TRUE,
                 NULL);

    // [5/8] Configure low-latency queue
    g_object_set(G_OBJECT(gst_.queue),
                 "max-size-buffers", 1,
                 "max-size-bytes", 0,
                 "max-size-time", GST_MSECOND * 5,
                 "leaky", 2,
                 NULL);

    // [6/8] Add elements to pipeline (decodebin 動態 pad 不需要 link)
    gst_bin_add_many(GST_BIN(gst_.pipeline),
                     gst_.source, gst_.decodebin, gst_.video_convert, gst_.queue, gst_.video_sink, NULL);
    
	// [7/8] Link static elements: videoconvert -> queue -> fakesink
	GstCaps* caps = gst_caps_from_string("video/x-raw,format=RGBA");
	if (!gst_element_link_filtered(gst_.video_convert, gst_.queue, caps)) {
	  std::cerr << "Failed to link videoconvert -> queue (RGBA)" << std::endl;
	  gst_caps_unref(caps);
	  return false;
	}
	gst_caps_unref(caps);
	
    if (!gst_element_link(gst_.queue, gst_.video_sink)) {
	  std::cerr << "Failed to link queue -> fakesink" << std::endl;
	  return false;
	}

    // [8/8] Connect dynamic pads
    // rtspsrc -> decodebin
    g_signal_connect(gst_.source, "pad-added", G_CALLBACK(+[](GstElement* src, GstPad* new_pad, gpointer user_data){
        GstElement* decodebin = static_cast<GstElement*>(user_data);
        GstPad* sink_pad = gst_element_get_static_pad(decodebin, "sink");
        if (!gst_pad_is_linked(sink_pad)) {
            gst_pad_link(new_pad, sink_pad);
        }
        gst_object_unref(sink_pad);
    }), gst_.decodebin);

    // decodebin -> videoconvert
    g_signal_connect(gst_.decodebin, "pad-added", G_CALLBACK(+[](GstElement* src, GstPad* new_pad, gpointer user_data){
        GstElement* video_convert = static_cast<GstElement*>(user_data);
        GstPad* sink_pad = gst_element_get_static_pad(video_convert, "sink");
        if (!gst_pad_is_linked(sink_pad)) {
            gst_pad_link(new_pad, sink_pad);
        }
        gst_object_unref(sink_pad);
    }), gst_.video_convert);

    // Connect fakesink handoff signal
    g_signal_connect(gst_.video_sink, "handoff", G_CALLBACK(HandoffHandler), this);

    // Set up pipeline bus and sync handler
    gst_.bus = gst_pipeline_get_bus(GST_PIPELINE(gst_.pipeline));
    if (!gst_.bus) {
        std::cerr << "Failed to get pipeline bus" << std::endl;
        return false;
    }
    gst_bus_set_sync_handler(gst_.bus, HandleGstMessage, this, NULL);

    std::cout << "RTSP decodebin pipeline created successfully for: " << uri_.c_str() << std::endl;
    return true;
}

// Creats a video pipeline using playbin.
// $ playbin uri=<file> video-sink="videoconvert ! video/x-raw,format=RGBA !
// fakesink"
bool GstVideoPlayer::CreateAutoDecodeFilePipeline() {
  gst_.pipeline = gst_pipeline_new("pipeline");
  if (!gst_.pipeline) {
    std::cerr << "Failed to create a pipeline" << std::endl;
    return false;
  }
  gst_.playbin = gst_element_factory_make("playbin", "playbin");
  if (!gst_.playbin) {
    std::cerr << "Failed to create a source" << std::endl;
    return false;
  }
  gst_.video_convert = gst_element_factory_make("videoconvert", "videoconvert");
  if (!gst_.video_convert) {
    std::cerr << "Failed to create a videoconvert" << std::endl;
    return false;
  }
  gst_.video_sink = gst_element_factory_make("fakesink", "videosink");
  if (!gst_.video_sink) {
    std::cerr << "Failed to create a videosink" << std::endl;
    return false;
  }
  gst_.output = gst_bin_new("output");
  if (!gst_.output) {
    std::cerr << "Failed to create an output" << std::endl;
    return false;
  }
  gst_.bus = gst_pipeline_get_bus(GST_PIPELINE(gst_.pipeline));
  if (!gst_.bus) {
    std::cerr << "Failed to create a bus" << std::endl;
    return false;
  }
  gst_bus_set_sync_handler(gst_.bus, HandleGstMessage, this, NULL);

  // Sets properties to fakesink to get the callback of a decoded frame.
  g_object_set(G_OBJECT(gst_.video_sink), "sync", TRUE, "qos", FALSE, NULL);
  g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", TRUE, NULL);
  g_signal_connect(G_OBJECT(gst_.video_sink), "handoff",
                   G_CALLBACK(HandoffHandler), this);
  gst_bin_add_many(GST_BIN(gst_.output), gst_.video_convert, gst_.video_sink,
                   NULL);

  // Adds caps to the converter to convert the color format to RGBA.
  auto* caps = gst_caps_from_string("video/x-raw,format=RGBA");
  auto link_ok =
      gst_element_link_filtered(gst_.video_convert, gst_.video_sink, caps);
  gst_caps_unref(caps);
  if (!link_ok) {
    std::cerr << "Failed to link elements" << std::endl;
    return false;
  }

  auto* sinkpad = gst_element_get_static_pad(gst_.video_convert, "sink");
  auto* ghost_sinkpad = gst_ghost_pad_new("sink", sinkpad);
  gst_pad_set_active(ghost_sinkpad, TRUE);
  gst_element_add_pad(gst_.output, ghost_sinkpad);
  gst_object_unref(sinkpad);

  // Sets properties to playbin.
  g_object_set(gst_.playbin, "uri", uri_.c_str(), NULL);
  g_object_set(gst_.playbin, "video-sink", gst_.output, NULL);
  gst_bin_add_many(GST_BIN(gst_.pipeline), gst_.playbin, NULL);

  return true;
}

bool GstVideoPlayer::Preroll() {
  if (is_rtsp_) {
    if (!gst_.pipeline) {
      return false;
    }
  } else {
    if (!gst_.playbin) {
      return false;
    }
  }

  auto result = gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED);
  if (result == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PAUSED" << std::endl;
    return false;
  }

  // Waits until the state becomes GST_STATE_PAUSED.
  if (result == GST_STATE_CHANGE_ASYNC) {
    GstState state, pending;
    result = gst_element_get_state(gst_.pipeline, &state, &pending, GST_CLOCK_TIME_NONE);
    if (result == GST_STATE_CHANGE_FAILURE) {
      return false;
    }
  }
  
  return true;
}

void GstVideoPlayer::DestroyPipeline() {
  if (gst_.video_sink) {
    g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", FALSE, NULL);
  }

  if (gst_.pipeline) {
    gst_element_set_state(gst_.pipeline, GST_STATE_NULL);
  }

  if (gst_.buffer) {
    gst_buffer_unref(gst_.buffer);
    gst_.buffer = nullptr;
  }

  if (gst_.bus) {
    gst_object_unref(gst_.bus);
    gst_.bus = nullptr;
  }

  if (gst_.pipeline) {
    gst_object_unref(gst_.pipeline);
    gst_.pipeline = nullptr;
  }

  if (gst_.playbin) {
    gst_.playbin = nullptr;
  }

  if (gst_.output) {
    gst_.output = nullptr;
  }

  if (gst_.video_sink) {
    gst_.video_sink = nullptr;
  }

  if (gst_.video_convert) {
    gst_.video_convert = nullptr;
  }

  // Unreference and clear the decoder
  if (gst_.decodebin) {
    gst_.decodebin = nullptr;
  }

  // Unreference and clear the parse
  if (gst_.parse) {
    gst_.parse = nullptr;
  }

  // Unreference and clear the depay
  if (gst_.depay) {
    gst_.depay = nullptr;
  }

  // Unreference and clear the source
  if (gst_.source) {
    gst_.source = nullptr;
  }
}

std::string GstVideoPlayer::ParseUri(const std::string& uri) {
  if (gst_uri_is_valid(uri.c_str())) {
    return uri;
  }

  auto* filename_uri = gst_filename_to_uri(uri.c_str(), NULL);
  if (!filename_uri) {
    std::cerr << "Faild to open " << uri.c_str() << std::endl;
    return uri;
  }
  std::string result_uri(filename_uri);
  g_free(filename_uri);

  return result_uri;
}

void GstVideoPlayer::GetVideoSize(int32_t& width, int32_t& height) {
  width = 0;
  height = 0;

  if (!gst_.pipeline || !gst_.video_sink) {
    std::cerr
        << "Failed to get video size. The pileline hasn't initialized yet.";
    return;
  }
	
  if (is_rtsp_) {
    std::cerr
        << "rtsp width " << width << ", height " << height << std::endl;
    if (width <= 0 || height <= 0 || width > MAX_WIDTH || height > MAX_HEIGHT) {
      width = 0;
      height = 0;
      return;
    }
  }
  
  auto* sink_pad = gst_element_get_static_pad(gst_.video_sink, "sink");
  if (!sink_pad) {
    std::cerr << "Failed to get a pad";
    return;
  }

  auto* caps = gst_pad_get_current_caps(sink_pad);
  if (!caps || gst_caps_get_size(caps) == 0) {
    std::cerr << "Failed to get caps" << std::endl;
    gst_object_unref(sink_pad);
    return;
  }

  auto* structure = gst_caps_get_structure(caps, 0);
  if (!structure) {
    std::cerr << "Failed to get a structure" << std::endl;
    gst_caps_unref(caps);
    gst_object_unref(sink_pad);
    return;
  }

  gst_structure_get_int(structure, "width", &width);
  gst_structure_get_int(structure, "height", &height);

#ifdef USE_EGL_IMAGE_DMABUF
  gboolean res = gst_video_info_from_caps(&gst_video_info_, caps);
  if (!res) {
    std::cerr << "Failed to get a gst_video_info" << std::endl;
    gst_caps_unref(caps);
    gst_object_unref(sink_pad);
    return;
  }
#endif  // USE_EGL_IMAGE_DMABUF

  gst_caps_unref(caps);
  gst_object_unref(sink_pad);
}

void GstVideoPlayer::onPadAdded(GstElement *src, GstPad *new_pad, GstElement *depay)
{
  std::cout << "test onPadAdded" << std::endl;
  GstPad *sink_pad = gst_element_get_static_pad(depay, "sink");
  if (!gst_pad_is_linked(sink_pad)) {
    GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
    if (ret != GST_PAD_LINK_OK) {
      std::cerr << "Failed to link dynamic pad from source to depayloader" << std::endl;
    }
  }
  gst_object_unref(sink_pad);
}

// static
void GstVideoPlayer::HandoffHandler(GstElement* fakesink, GstBuffer* buf,
                                    GstPad* new_pad, gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  auto* caps = gst_pad_get_current_caps(new_pad);
  if (!caps || gst_caps_get_size(caps) == 0) {
    std::cerr << "[Handoff] Failed to get valid caps" << std::endl;
    return;
  }
  auto* structure = gst_caps_get_structure(caps, 0);
  if (!structure) {
    std::cerr << "[Handoff] Caps has no structure" << std::endl;
    gst_caps_unref(caps);
    return;
  }

  int width = 0, height = 0;
  if (!gst_structure_get_int(structure, "width", &width) || !gst_structure_get_int(structure, "height", &height)) {
    gst_caps_unref(caps);
    return;
  }
  gst_caps_unref(caps);
  if (width != self->width_ || height != self->height_) {
    self->width_ = width;
    self->height_ = height;
    try {
	    self->pixels_.reset(new uint32_t[width * height]);
      std::cout << "Pixel buffer size: width = " << width
              << ", height = " << height << std::endl;

      self->stream_handler_->OnNotifyInitialized();
	  } catch (const std::bad_alloc &e) {
      return;
    }
  }

  {
    std::lock_guard<std::shared_mutex> lock(self->mutex_buffer_);
    if (self->gst_.buffer) {
      gst_buffer_unref(self->gst_.buffer);
      self->gst_.buffer = nullptr;
    }
    self->gst_.buffer = gst_buffer_ref(buf);
  }
  self->stream_handler_->OnNotifyFrameDecoded();
}

// static
GstBusSyncReply GstVideoPlayer::HandleGstMessage(GstBus* bus,
                                                 GstMessage* message,
                                                 gpointer user_data) {
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS: {
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      std::lock_guard<std::mutex> lock(self->mutex_event_completed_);
      self->is_completed_ = true;
      break;
    }
    case GST_MESSAGE_WARNING: {
      gchar* debug;
      GError* error;
      gst_message_parse_warning(message, &error, &debug);
      g_printerr("WARNING from element %s: %s\n", GST_OBJECT_NAME(message->src),
                 error->message);
      g_printerr("Warning details: %s\n", debug);
      g_free(debug);
      g_error_free(error);
      break;
    }
    case GST_MESSAGE_ERROR: {
      gchar* debug;
      GError* error;
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

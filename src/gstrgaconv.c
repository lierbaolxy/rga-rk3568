/*
 * gstrgaconv.c - Minimal Rockchip RGA hardware color-convert element
 *
 * NV12/NV16/I420/NV21 (sysmem) -> BGRx/BGRA/RGBx/RGBA (sysmem), same size.
 * Uses librga im2d synchronous blit (hardware, ~2-4ms per 1080p frame).
 *
 * Build:
 *   gcc -shared -fPIC -O2 -DPACKAGE='"rgaconv"' -DVERSION='"1.0"' \
 *     -o libgstrgaconv.so gstrgaconv.c \
 *     $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0 \
 *       gstreamer-video-1.0 librga)
 * Install:
 *   sudo cp libgstrgaconv.so /usr/lib/aarch64-linux-gnu/gstreamer-1.0/
 */
#include <stdio.h>
#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>

#include <rga.h>
#include <im2d.h>
#include <im2d_single.h>
#include <im2d_buffer.h>

GST_DEBUG_CATEGORY_STATIC (gst_rgaconv_debug);
#define GST_CAT_DEFAULT gst_rgaconv_debug

typedef struct _GstRgaConv GstRgaConv;
typedef struct _GstRgaConvClass GstRgaConvClass;

struct _GstRgaConv
{
  GstBaseTransform parent;
  GstVideoInfo in_info;
  GstVideoInfo out_info;
  gint in_rga_fmt;
  gint out_rga_fmt;
};

struct _GstRgaConvClass
{
  GstBaseTransformClass parent_class;
};

#define gst_rgaconv_parent_class parent_class
G_DEFINE_TYPE (GstRgaConv, gst_rgaconv, GST_TYPE_BASE_TRANSFORM);

#define SINK_CAPS \
  "video/x-raw, format = (string) { NV12, NV16, I420, NV21 }, " \
  "width = (int) [ 16, 8192 ], height = (int) [ 16, 8192 ], " \
  "framerate = (fraction) [ 0/1, 2147483647/1 ]"
#define SRC_CAPS \
  "video/x-raw, format = (string) { BGRx, BGRA, RGBx, RGBA }, " \
  "width = (int) [ 16, 8192 ], height = (int) [ 16, 8192 ], " \
  "framerate = (fraction) [ 0/1, 2147483647/1 ]"

static gint
gst_to_rga_format (GstVideoFormat fmt)
{
  switch (fmt) {
    case GST_VIDEO_FORMAT_NV12: return RK_FORMAT_YCbCr_420_SP;
    case GST_VIDEO_FORMAT_NV21: return RK_FORMAT_YCrCb_420_SP;
    case GST_VIDEO_FORMAT_I420: return RK_FORMAT_YCbCr_420_P;
    case GST_VIDEO_FORMAT_NV16: return RK_FORMAT_YCbCr_422_SP;
    case GST_VIDEO_FORMAT_BGRx: return RK_FORMAT_BGRX_8888;
    case GST_VIDEO_FORMAT_BGRA: return RK_FORMAT_BGRA_8888;
    case GST_VIDEO_FORMAT_RGBx: return RK_FORMAT_RGBX_8888;
    case GST_VIDEO_FORMAT_RGBA: return RK_FORMAT_RGBA_8888;
    default: return RK_FORMAT_UNKNOWN;
  }
}

static GstCaps *
gst_rgaconv_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *templ, *result, *inter;
  gint i, n;

  /* direction tells which pad the INPUT caps belong to; we must produce
   * caps for the OTHER pad */
  templ = gst_caps_from_string (direction == GST_PAD_SINK ? SRC_CAPS : SINK_CAPS);
  result = gst_caps_new_empty ();
  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    GstStructure *in_st = gst_caps_get_structure (caps, i);
    GstStructure *out_st = gst_structure_copy (in_st);
    /* drop input format so the other side's template formats survive the
     * intersect (input NV12 vs output RGB would otherwise yield EMPTY) */
    gst_structure_remove_field (out_st, "format");
    /* replace format field by the other side's template formats */
    GstCaps *tmp = gst_caps_new_empty ();
    gst_caps_append_structure (tmp, out_st);
    inter = gst_caps_intersect_full (tmp, templ, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (tmp); /* owns out_st now */
    gst_caps_append (result, inter);
  }
  gst_caps_unref (templ);

  if (filter) {
    inter = gst_caps_intersect_full (result, filter, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (result);
    result = inter;
  }

  /* keep only fields both sides understand: rebuild clean structures */
  {
    GstCaps *clean = gst_caps_new_empty ();
    n = gst_caps_get_size (result);
    for (i = 0; i < n; i++) {
      const GstStructure *st = gst_caps_get_structure (result, i);
      GstStructure *ns = gst_structure_new_empty ("video/x-raw");
      const GValue *fmt = gst_structure_get_value (st, "format");
      const GValue *w = gst_structure_get_value (st, "width");
      const GValue *h = gst_structure_get_value (st, "height");
      const GValue *fps = gst_structure_get_value (st, "framerate");
      if (fmt) gst_structure_set_value (ns, "format", fmt);
      if (w) gst_structure_set_value (ns, "width", w);
      if (h) gst_structure_set_value (ns, "height", h);
      if (fps) gst_structure_set_value (ns, "framerate", fps);
      gst_caps_append_structure (clean, ns);
    }
    gst_caps_unref (result);
    result = clean;
  }
  return result;
}

static gboolean
gst_rgaconv_transform_size (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, gsize size, GstCaps * othercaps,
    gsize * othersize)
{
  GstVideoInfo info;
  if (gst_video_info_from_caps (&info, othercaps)) {
    *othersize = info.size;
    return TRUE;
  }
  *othersize = size;
  return TRUE;
}

static gboolean
gst_rgaconv_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstRgaConv *self = (GstRgaConv *) trans;

  if (!gst_video_info_from_caps (&self->in_info, incaps))
    return FALSE;
  if (!gst_video_info_from_caps (&self->out_info, outcaps))
    return FALSE;
  if (GST_VIDEO_INFO_WIDTH (&self->in_info) != GST_VIDEO_INFO_WIDTH (&self->out_info)
      || GST_VIDEO_INFO_HEIGHT (&self->in_info) != GST_VIDEO_INFO_HEIGHT (&self->out_info))
    return FALSE;

  self->in_rga_fmt = gst_to_rga_format (GST_VIDEO_INFO_FORMAT (&self->in_info));
  self->out_rga_fmt = gst_to_rga_format (GST_VIDEO_INFO_FORMAT (&self->out_info));
  if (self->in_rga_fmt == RK_FORMAT_UNKNOWN || self->out_rga_fmt == RK_FORMAT_UNKNOWN)
    return FALSE;

  GST_INFO_OBJECT (self, "RGA convert %s -> %s %dx%d",
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&self->in_info)),
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&self->out_info)),
      GST_VIDEO_INFO_WIDTH (&self->in_info), GST_VIDEO_INFO_HEIGHT (&self->in_info));
  return TRUE;
}

static GstFlowReturn
gst_rgaconv_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstRgaConv *self = (GstRgaConv *) trans;
  GstVideoFrame in_frame, out_frame;
  rga_buffer_t src_img, dst_img;
  IM_STATUS ret;
  GstFlowReturn flow = GST_FLOW_OK;
  gint w, h;

  if (!gst_video_frame_map (&in_frame, &self->in_info, inbuf, GST_MAP_READ))
    return GST_FLOW_ERROR;
  if (!gst_video_frame_map (&out_frame, &self->out_info, outbuf, GST_MAP_WRITE)) {
    gst_video_frame_unmap (&in_frame);
    return GST_FLOW_ERROR;
  }

  w = GST_VIDEO_INFO_WIDTH (&self->in_info);
  h = GST_VIDEO_INFO_HEIGHT (&self->in_info);

  /* Use the REAL plane stride/offset from the mapped frame: MPP aligns
   * frames to 16 lines (e.g. 1080 -> 1088), tight-packing assumptions
   * misplace the chroma plane and corrupt colors. */
  {
    gint s_wstr, s_hstr, d_wstr, d_hstr;

    s_wstr = GST_VIDEO_INFO_PLANE_STRIDE (&in_frame.info, 0);   /* 1px==1B for 8bit YUV */
    s_hstr = s_wstr ? GST_VIDEO_INFO_PLANE_OFFSET (&in_frame.info, 1) / s_wstr : h;
    d_wstr = GST_VIDEO_INFO_PLANE_STRIDE (&out_frame.info, 0) / 4; /* px, 4B/px RGB */
    d_hstr = GST_VIDEO_INFO_HEIGHT (&out_frame.info);

    src_img = wrapbuffer_virtualaddr_t (GST_VIDEO_FRAME_PLANE_DATA (&in_frame, 0),
                w, h, s_wstr, s_hstr, self->in_rga_fmt);
    dst_img = wrapbuffer_virtualaddr_t (GST_VIDEO_FRAME_PLANE_DATA (&out_frame, 0),
                w, h, d_wstr, d_hstr, self->out_rga_fmt);
  }

  ret = imcvtcolor (src_img, dst_img, self->in_rga_fmt, self->out_rga_fmt);
  if (ret != IM_STATUS_SUCCESS) {
    GST_ERROR_OBJECT (self, "imcvtcolor failed: %s", imStrError (ret));
    flow = GST_FLOW_ERROR;
  }

  gst_video_frame_unmap (&out_frame);
  gst_video_frame_unmap (&in_frame);
  return flow;
}

static void
gst_rgaconv_class_init (GstRgaConvClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *bt_class = GST_BASE_TRANSFORM_CLASS (klass);
  GstCaps *sink_caps, *src_caps;

  gst_element_class_set_static_metadata (element_class,
      "Rockchip RGA color converter",
      "Filter/Converter/Video",
      "Hardware color conversion via RGA (YUV->RGB)",
      "local");

  sink_caps = gst_caps_from_string (SINK_CAPS);
  src_caps = gst_caps_from_string (SRC_CAPS);
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sink_caps));
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, src_caps));
  gst_caps_unref (sink_caps);
  gst_caps_unref (src_caps);

  bt_class->transform_caps = GST_DEBUG_FUNCPTR (gst_rgaconv_transform_caps);
  bt_class->transform_size = GST_DEBUG_FUNCPTR (gst_rgaconv_transform_size);
  bt_class->set_caps = GST_DEBUG_FUNCPTR (gst_rgaconv_set_caps);
  bt_class->transform = GST_DEBUG_FUNCPTR (gst_rgaconv_transform);
  bt_class->passthrough_on_same_caps = FALSE;
}

static void
gst_rgaconv_init (GstRgaConv * self)
{
  gst_base_transform_set_qos_enabled (GST_BASE_TRANSFORM (self), TRUE);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_rgaconv_debug, "rgaconv", 0, "RGA converter");
  return gst_element_register (plugin, "rgaconv", GST_RANK_NONE,
      gst_rgaconv_get_type ());
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR,
    rgaconv, "Rockchip RGA color converter",
    plugin_init, "1.0", "LGPL", "rgaconv", "local")

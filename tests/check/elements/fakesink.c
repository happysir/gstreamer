/* GStreamer
 *
 * unit test for fakesink
 *
 * Copyright (C) <2005> Thomas Vander Stichele <thomas at apestaart dot org>
 *               <2007> Wim Taymans <wim@fluendo.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <unistd.h>

#include <gst/check/gstcheck.h>

typedef struct
{
  GstPad *pad;
  GstBuffer *buffer;
  GThread *thread;
  GstFlowReturn ret;
} ChainData;

static gpointer
chain_async_buffer (gpointer data)
{
  ChainData *chain_data = (ChainData *) data;

  chain_data->ret = gst_pad_chain (chain_data->pad, chain_data->buffer);

  return chain_data;
}

static ChainData *
chain_async (GstPad * pad, GstBuffer * buffer)
{
  GThread *thread;
  ChainData *chain_data;
  GError *error = NULL;

  chain_data = g_new (ChainData, 1);
  chain_data->pad = pad;
  chain_data->buffer = buffer;
  chain_data->ret = GST_FLOW_ERROR;

  thread = g_thread_create (chain_async_buffer, chain_data, TRUE, &error);
  if (error != NULL) {
    g_warning ("could not create thread reason: %s", error->message);
    g_free (chain_data);
    return NULL;
  }
  chain_data->thread = thread;

  return chain_data;
}

static GstFlowReturn
chain_async_return (ChainData * data)
{
  GstFlowReturn ret;

  g_thread_join (data->thread);
  ret = data->ret;
  g_free (data);

  return ret;
}

GST_START_TEST (test_clipping)
{
  GstElement *sink;
  GstPad *sinkpad;
  GstStateChangeReturn ret;

  /* create sink */
  sink = gst_element_factory_make ("fakesink", "sink");
  fail_if (sink == NULL);

  sinkpad = gst_element_get_static_pad (sink, "sink");
  fail_if (sinkpad == NULL);

  /* make element ready to accept data */
  ret = gst_element_set_state (sink, GST_STATE_PAUSED);
  fail_unless (ret == GST_STATE_CHANGE_ASYNC);

  /* send segment */
  {
    GstEvent *segment;
    gboolean eret;

    GST_DEBUG ("sending segment");
    segment = gst_event_new_new_segment (FALSE,
        1.0, GST_FORMAT_TIME, 1 * GST_SECOND, 5 * GST_SECOND, 1 * GST_SECOND);

    eret = gst_pad_send_event (sinkpad, segment);
    fail_if (eret == FALSE);
  }

  /* new segment should not have finished preroll */
  ret = gst_element_get_state (sink, NULL, NULL, 0);
  fail_unless (ret == GST_STATE_CHANGE_ASYNC);

  /* send buffer that should be dropped */
  {
    GstBuffer *buffer;
    GstFlowReturn fret;

    buffer = gst_buffer_new ();
    GST_BUFFER_TIMESTAMP (buffer) = 0;
    GST_BUFFER_DURATION (buffer) = 1 * GST_MSECOND;

    GST_DEBUG ("sending buffer to be dropped");
    fret = gst_pad_chain (sinkpad, buffer);
    fail_if (fret != GST_FLOW_OK);
  }
  /* dropped buffer should not have finished preroll */
  ret = gst_element_get_state (sink, NULL, NULL, 0);
  fail_unless (ret == GST_STATE_CHANGE_ASYNC);

  /* send buffer that should be dropped */
  {
    GstBuffer *buffer;
    GstFlowReturn fret;

    buffer = gst_buffer_new ();
    GST_BUFFER_TIMESTAMP (buffer) = 5 * GST_SECOND;
    GST_BUFFER_DURATION (buffer) = 1 * GST_MSECOND;

    GST_DEBUG ("sending buffer to be dropped");
    fret = gst_pad_chain (sinkpad, buffer);
    fail_if (fret != GST_FLOW_OK);
  }
  /* dropped buffer should not have finished preroll */
  ret = gst_element_get_state (sink, NULL, NULL, 0);
  fail_unless (ret == GST_STATE_CHANGE_ASYNC);

  /* send buffer that should block and finish preroll */
  {
    GstBuffer *buffer;
    GstFlowReturn fret;
    ChainData *data;
    GstState current, pending;

    buffer = gst_buffer_new ();
    GST_BUFFER_TIMESTAMP (buffer) = 1 * GST_SECOND;
    GST_BUFFER_DURATION (buffer) = 1 * GST_MSECOND;

    GST_DEBUG ("sending buffer to finish preroll");
    data = chain_async (sinkpad, buffer);
    fail_if (data == NULL);

    /* state should now eventually change to PAUSED */
    ret = gst_element_get_state (sink, &current, &pending, GST_CLOCK_TIME_NONE);
    fail_unless (ret == GST_STATE_CHANGE_SUCCESS);
    fail_unless (current == GST_STATE_PAUSED);
    fail_unless (pending == GST_STATE_VOID_PENDING);

    /* playing should render the buffer */
    ret = gst_element_set_state (sink, GST_STATE_PLAYING);
    fail_unless (ret == GST_STATE_CHANGE_SUCCESS);

    /* and we should get a success return value */
    fret = chain_async_return (data);
    fail_if (fret != GST_FLOW_OK);
  }

  /* send some buffer that will be dropped or clipped, this can 
   * only be observed in the debug log. */
  {
    GstBuffer *buffer;
    GstFlowReturn fret;

    buffer = gst_buffer_new ();
    GST_BUFFER_TIMESTAMP (buffer) = 6 * GST_SECOND;
    GST_BUFFER_DURATION (buffer) = 1 * GST_MSECOND;

    /* should be dropped */
    GST_DEBUG ("sending buffer to drop");
    fret = gst_pad_chain (sinkpad, buffer);
    fail_if (fret != GST_FLOW_OK);

    buffer = gst_buffer_new ();
    GST_BUFFER_TIMESTAMP (buffer) = 0 * GST_SECOND;
    GST_BUFFER_DURATION (buffer) = 2 * GST_SECOND;

    /* should be clipped */
    GST_DEBUG ("sending buffer to clip");
    fret = gst_pad_chain (sinkpad, buffer);
    fail_if (fret != GST_FLOW_OK);

    buffer = gst_buffer_new ();
    GST_BUFFER_TIMESTAMP (buffer) = 4 * GST_SECOND;
    GST_BUFFER_DURATION (buffer) = 2 * GST_SECOND;

    /* should be clipped */
    GST_DEBUG ("sending buffer to clip");
    fret = gst_pad_chain (sinkpad, buffer);
    fail_if (fret != GST_FLOW_OK);
  }

  gst_element_set_state (sink, GST_STATE_NULL);
  gst_element_get_state (sink, NULL, NULL, GST_CLOCK_TIME_NONE);
  gst_object_unref (sinkpad);
  gst_object_unref (sink);
}

GST_END_TEST;

GST_START_TEST (test_preroll_sync)
{
  GstElement *pipeline, *sink;
  GstPad *sinkpad;
  GstStateChangeReturn ret;

  /* create sink */
  pipeline = gst_pipeline_new ("pipeline");
  fail_if (pipeline == NULL);

  sink = gst_element_factory_make ("fakesink", "sink");
  fail_if (sink == NULL);
  g_object_set (G_OBJECT (sink), "sync", TRUE, NULL);

  gst_bin_add (GST_BIN (pipeline), sink);

  sinkpad = gst_element_get_static_pad (sink, "sink");
  fail_if (sinkpad == NULL);

  /* make pipeline and element ready to accept data */
  ret = gst_element_set_state (pipeline, GST_STATE_PAUSED);
  fail_unless (ret == GST_STATE_CHANGE_ASYNC);

  /* send segment */
  {
    GstEvent *segment;
    gboolean eret;

    GST_DEBUG ("sending segment");
    segment = gst_event_new_new_segment (FALSE,
        1.0, GST_FORMAT_TIME, 0 * GST_SECOND, 2 * GST_SECOND, 0 * GST_SECOND);

    eret = gst_pad_send_event (sinkpad, segment);
    fail_if (eret == FALSE);
  }

  /* send buffer that should block and finish preroll */
  {
    GstBuffer *buffer;
    GstFlowReturn fret;
    ChainData *data;
    GstState current, pending;

    buffer = gst_buffer_new ();
    GST_BUFFER_TIMESTAMP (buffer) = 1 * GST_SECOND;
    GST_BUFFER_DURATION (buffer) = 1 * GST_SECOND;

    GST_DEBUG ("sending buffer to finish preroll");
    data = chain_async (sinkpad, buffer);
    fail_if (data == NULL);

    /* state should now eventually change to PAUSED */
    ret =
        gst_element_get_state (pipeline, &current, &pending,
        GST_CLOCK_TIME_NONE);
    fail_unless (ret == GST_STATE_CHANGE_SUCCESS);
    fail_unless (current == GST_STATE_PAUSED);
    fail_unless (pending == GST_STATE_VOID_PENDING);

    /* playing should render the buffer */
    ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
    fail_unless (ret == GST_STATE_CHANGE_SUCCESS);

    /* and we should get a success return value */
    fret = chain_async_return (data);
    fail_if (fret != GST_FLOW_OK);
  }
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_element_get_state (pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
  gst_object_unref (sinkpad);
  gst_object_unref (pipeline);
}

GST_END_TEST;

/* after EOS, we refuse everything */
GST_START_TEST (test_eos)
{
  GstElement *pipeline, *sink;
  GstPad *sinkpad;
  GstStateChangeReturn ret;
  GstMessage *message;
  GstBus *bus;

  /* create sink */
  pipeline = gst_pipeline_new ("pipeline");
  fail_if (pipeline == NULL);

  bus = gst_pipeline_get_bus (GST_PIPELINE_CAST (pipeline));
  fail_if (bus == NULL);

  sink = gst_element_factory_make ("fakesink", "sink");
  fail_if (sink == NULL);
  g_object_set (G_OBJECT (sink), "sync", TRUE, NULL);

  gst_bin_add (GST_BIN (pipeline), sink);

  sinkpad = gst_element_get_static_pad (sink, "sink");
  fail_if (sinkpad == NULL);

  /* make pipeline and element ready to accept data */
  ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  fail_unless (ret == GST_STATE_CHANGE_ASYNC);

  /* send EOS, this should work fine */
  {
    GstEvent *eos;
    gboolean eret;

    GST_DEBUG ("sending EOS");
    eos = gst_event_new_eos ();

    eret = gst_pad_send_event (sinkpad, eos);
    fail_if (eret == FALSE);
  }

  /* wait for preroll */
  gst_element_get_state (pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

  /* EOS should be on the bus at some point */
  while (TRUE) {
    GstMessageType type;

    /* blocking wait for messages */
    message = gst_bus_timed_pop (bus, GST_CLOCK_TIME_NONE);
    type = GST_MESSAGE_TYPE (message);
    gst_message_unref (message);

    GST_DEBUG ("got message %s", gst_message_type_get_name (type));

    if (type == GST_MESSAGE_EOS)
      break;
  }
  gst_object_unref (bus);

  /* send another EOS, this should fail */
  {
    GstEvent *eos;
    gboolean eret;

    GST_DEBUG ("sending second EOS");
    eos = gst_event_new_eos ();

    eret = gst_pad_send_event (sinkpad, eos);
    fail_if (eret == TRUE);
  }

  /* send segment, this should fail */
  {
    GstEvent *segment;
    gboolean eret;

    GST_DEBUG ("sending segment");
    segment = gst_event_new_new_segment (FALSE,
        1.0, GST_FORMAT_TIME, 0 * GST_SECOND, 2 * GST_SECOND, 0 * GST_SECOND);

    eret = gst_pad_send_event (sinkpad, segment);
    fail_if (eret == TRUE);
  }

  /* send buffer that should fail after EOS */
  {
    GstBuffer *buffer;
    GstFlowReturn fret;

    buffer = gst_buffer_new ();
    GST_BUFFER_TIMESTAMP (buffer) = 1 * GST_SECOND;
    GST_BUFFER_DURATION (buffer) = 1 * GST_SECOND;

    GST_DEBUG ("sending buffer");

    /* buffer after EOS is not UNEXPECTED */
    fret = gst_pad_chain (sinkpad, buffer);
    fail_unless (fret == GST_FLOW_UNEXPECTED);
  }

  /* flush, EOS state is flushed again. */
  {
    GstEvent *event;
    gboolean eret;

    GST_DEBUG ("sending FLUSH_START");
    event = gst_event_new_flush_start ();
    eret = gst_pad_send_event (sinkpad, event);
    fail_unless (eret == TRUE);

    GST_DEBUG ("sending FLUSH_STOP");
    event = gst_event_new_flush_stop ();
    eret = gst_pad_send_event (sinkpad, event);
    fail_unless (eret == TRUE);
  }

  /* send segment, this should now work again */
  {
    GstEvent *segment;
    gboolean eret;

    GST_DEBUG ("sending segment");
    segment = gst_event_new_new_segment (FALSE,
        1.0, GST_FORMAT_TIME, 0 * GST_SECOND, 2 * GST_SECOND, 0 * GST_SECOND);

    eret = gst_pad_send_event (sinkpad, segment);
    fail_unless (eret == TRUE);
  }

  /* send buffer that should work and block */
  {
    GstBuffer *buffer;
    GstFlowReturn fret;

    buffer = gst_buffer_new ();
    GST_BUFFER_TIMESTAMP (buffer) = 1 * GST_SECOND;
    GST_BUFFER_DURATION (buffer) = 1 * GST_SECOND;

    GST_DEBUG ("sending buffer");

    fret = gst_pad_chain (sinkpad, buffer);
    fail_unless (fret == GST_FLOW_OK);
  }

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_element_get_state (pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
  gst_object_unref (sinkpad);
  gst_object_unref (pipeline);
}

GST_END_TEST;

/* test EOS triggered by the element */
GST_START_TEST (test_eos2)
{
  GstElement *pipeline, *sink;
  GstPad *sinkpad;
  GstStateChangeReturn ret;

  /* create sink */
  pipeline = gst_pipeline_new ("pipeline");
  fail_if (pipeline == NULL);

  sink = gst_element_factory_make ("fakesink", "sink");
  fail_if (sink == NULL);
  g_object_set (G_OBJECT (sink), "sync", TRUE, NULL);
  g_object_set (G_OBJECT (sink), "num-buffers", 1, NULL);

  gst_bin_add (GST_BIN (pipeline), sink);

  sinkpad = gst_element_get_static_pad (sink, "sink");
  fail_if (sinkpad == NULL);

  /* make pipeline and element ready to accept data */
  ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  fail_unless (ret == GST_STATE_CHANGE_ASYNC);

  /* send segment, this should work */
  {
    GstEvent *segment;
    gboolean eret;

    GST_DEBUG ("sending segment");
    segment = gst_event_new_new_segment (FALSE,
        1.0, GST_FORMAT_TIME, 0 * GST_SECOND, 2 * GST_SECOND, 0 * GST_SECOND);

    eret = gst_pad_send_event (sinkpad, segment);
    fail_if (eret == FALSE);
  }

  /* send buffer that should return UNEXPECTED */
  {
    GstBuffer *buffer;
    GstFlowReturn fret;

    buffer = gst_buffer_new ();
    GST_BUFFER_TIMESTAMP (buffer) = 1 * GST_SECOND;
    GST_BUFFER_DURATION (buffer) = 1 * GST_SECOND;

    GST_DEBUG ("sending buffer");

    /* this buffer will generate UNEXPECTED */
    fret = gst_pad_chain (sinkpad, buffer);
    fail_unless (fret == GST_FLOW_UNEXPECTED);
  }

  /* send buffer that should return UNEXPECTED */
  {
    GstBuffer *buffer;
    GstFlowReturn fret;

    buffer = gst_buffer_new ();
    GST_BUFFER_TIMESTAMP (buffer) = 1 * GST_SECOND;
    GST_BUFFER_DURATION (buffer) = 1 * GST_SECOND;

    GST_DEBUG ("sending buffer");

    fret = gst_pad_chain (sinkpad, buffer);
    fail_unless (fret == GST_FLOW_UNEXPECTED);
  }

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_element_get_state (pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
  gst_object_unref (sinkpad);
  gst_object_unref (pipeline);
}

GST_END_TEST;

/* test position reporting before, during and after flush 
 * in PAUSED and PLAYING */
GST_START_TEST (test_position)
{
  GstElement *pipeline, *sink;
  GstPad *sinkpad;
  GstStateChangeReturn ret;
  gboolean qret;
  GstFormat qformat;
  gint64 qcur;
  GstBuffer *buffer;
  GstFlowReturn fret;
  ChainData *data;
  GstEvent *event;
  gboolean eret;
  gint i;

  /* create sink */
  pipeline = gst_pipeline_new ("pipeline");
  fail_if (pipeline == NULL);

  sink = gst_element_factory_make ("fakesink", "sink");
  fail_if (sink == NULL);
  g_object_set (G_OBJECT (sink), "sync", TRUE, NULL);
  g_object_set (G_OBJECT (sink), "num-buffers", 2, NULL);

  gst_bin_add (GST_BIN (pipeline), sink);

  sinkpad = gst_element_get_static_pad (sink, "sink");
  fail_if (sinkpad == NULL);

  /* do position query, this should fail, we have nothing received yet */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == FALSE);

  ret = gst_element_set_state (pipeline, GST_STATE_READY);
  fail_unless (ret == GST_STATE_CHANGE_SUCCESS);

  /* do position query, this should fail, we have nothing received yet */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == FALSE);

  /* make pipeline and element ready to accept data */
  ret = gst_element_set_state (pipeline, GST_STATE_PAUSED);
  fail_unless (ret == GST_STATE_CHANGE_ASYNC);

  /* do position query, this should fail, we have nothing received yet */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == FALSE);

  /* send segment, this should work */
  {
    GST_DEBUG ("sending segment");
    event = gst_event_new_new_segment (FALSE,
        1.0, GST_FORMAT_TIME, 1 * GST_SECOND, 3 * GST_SECOND, 1 * GST_SECOND);

    eret = gst_pad_send_event (sinkpad, event);
    fail_if (eret == FALSE);
  }

  /* FIXME, do position query, this should succeed with the time value from the
   * segment. */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == TRUE);
  fail_unless (qcur == 1 * GST_SECOND);

  /* send buffer that we will flush out */
  buffer = gst_buffer_new ();
  GST_BUFFER_TIMESTAMP (buffer) = 2 * GST_SECOND;
  GST_BUFFER_DURATION (buffer) = 1 * GST_SECOND;

  GST_DEBUG ("sending buffer");

  /* this buffer causes the sink to preroll */
  data = chain_async (sinkpad, buffer);
  fail_if (data == NULL);

  /* wait for preroll */
  ret = gst_element_get_state (pipeline, NULL, NULL, -1);

  /* do position query, this should succeed with the time value from the
   * segment. */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == TRUE);
  fail_unless (qcur == 1 * GST_SECOND);

  /* start flushing, no timing is affected yet */
  {
    GST_DEBUG ("sending flush_start");
    event = gst_event_new_flush_start ();

    eret = gst_pad_send_event (sinkpad, event);
    fail_if (eret == FALSE);
  }

  /* preroll buffer is flushed out */
  fret = chain_async_return (data);
  fail_unless (fret == GST_FLOW_WRONG_STATE);

  /* do position query, this should succeed with the time value from the
   * segment before the flush. */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == TRUE);
  fail_unless (qcur == 1 * GST_SECOND);

  /* stop flushing, timing is affected now */
  {
    GST_DEBUG ("sending flush_stop");
    event = gst_event_new_flush_stop ();

    eret = gst_pad_send_event (sinkpad, event);
    fail_if (eret == FALSE);
  }

  /* do position query, this should fail, the segment is flushed */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == FALSE);

  /* send segment, this should work */
  {
    GST_DEBUG ("sending segment");
    event = gst_event_new_new_segment (FALSE,
        1.0, GST_FORMAT_TIME, 2 * GST_SECOND, 4 * GST_SECOND, 1 * GST_SECOND);

    eret = gst_pad_send_event (sinkpad, event);
    fail_if (eret == FALSE);
  }

  /* send buffer that should return OK */
  buffer = gst_buffer_new ();
  GST_BUFFER_TIMESTAMP (buffer) = 3 * GST_SECOND;
  GST_BUFFER_DURATION (buffer) = 1 * GST_SECOND;

  GST_DEBUG ("sending buffer");

  /* this buffer causes the sink to preroll */
  data = chain_async (sinkpad, buffer);
  fail_if (data == NULL);

  /* wait for preroll */
  ret = gst_element_get_state (pipeline, NULL, NULL, -1);

  /* do position query, this should succeed with the time value from the
   * segment. */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == TRUE);
  fail_unless (qcur == 1 * GST_SECOND);

  ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  fail_unless (ret == GST_STATE_CHANGE_SUCCESS);

  /* position now is increasing but never exceeds the boundaries of the segment */
  for (i = 0; i < 5; i++) {
    qformat = GST_FORMAT_TIME;
    qret = gst_element_query_position (sink, &qformat, &qcur);
    GST_DEBUG ("position %" GST_TIME_FORMAT, GST_TIME_ARGS (qcur));
    fail_unless (qret == TRUE);
    fail_unless (qcur >= 1 * GST_SECOND && qcur <= 3 * GST_SECOND);
    g_usleep (1000 * 250);
  }

  /* preroll buffer is rendered, we expect one more buffer after this one */
  fret = chain_async_return (data);
  fail_unless (fret == GST_FLOW_OK);

  /* after rendering the position must be bigger then the stream_time of the
   * buffer */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == TRUE);
  fail_unless (qcur >= 2 * GST_SECOND && qcur <= 3 * GST_SECOND);

  /* start flushing in PLAYING */
  {
    GST_DEBUG ("sending flush_start");
    event = gst_event_new_flush_start ();

    eret = gst_pad_send_event (sinkpad, event);
    fail_if (eret == FALSE);
  }

  /* this should now just report the stream time of the last buffer */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == TRUE);
  fail_unless (qcur == 2 * GST_SECOND);

  {
    GST_DEBUG ("sending flush_stop");
    event = gst_event_new_flush_stop ();

    eret = gst_pad_send_event (sinkpad, event);
    fail_if (eret == FALSE);
  }

  /* do position query, this should fail, the segment is flushed */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == FALSE);

  /* send segment, this should work */
  {
    GST_DEBUG ("sending segment");
    event = gst_event_new_new_segment (FALSE,
        1.0, GST_FORMAT_TIME, 2 * GST_SECOND, 4 * GST_SECOND, 1 * GST_SECOND);

    eret = gst_pad_send_event (sinkpad, event);
    fail_if (eret == FALSE);
  }

  /* send buffer that should return UNEXPECTED */
  buffer = gst_buffer_new ();
  GST_BUFFER_TIMESTAMP (buffer) = 3 * GST_SECOND;
  GST_BUFFER_DURATION (buffer) = 1 * GST_SECOND;

  GST_DEBUG ("sending buffer");

  /* this buffer causes the sink to preroll */
  data = chain_async (sinkpad, buffer);
  fail_if (data == NULL);

  /* wait for preroll */
  ret = gst_element_get_state (pipeline, NULL, NULL, -1);

  /* preroll buffer is rendered, we expect no more buffer after this one */
  fret = chain_async_return (data);
  fail_unless (fret == GST_FLOW_UNEXPECTED);

  /* do position query, this should succeed with the stream time of the buffer
   * against the clock. Since the buffer is synced against the clock, the time
   * should be at least the stream time of the buffer. */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == TRUE);
  fail_unless (qcur >= 2 * GST_SECOND && qcur <= 3 * GST_SECOND);

  /* wait 2 more seconds, enough to test if the position was clipped correctly
   * against the segment */
  g_usleep (2 * G_USEC_PER_SEC);

  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == TRUE);
  fail_unless (qcur == 3 * GST_SECOND);

  GST_DEBUG ("going to PAUSED");

  ret = gst_element_set_state (pipeline, GST_STATE_PAUSED);
  fail_unless (ret == GST_STATE_CHANGE_ASYNC);

  /* we report the time of the last start of the buffer. This is slightly
   * incorrect, we should report the exact time when we paused but there is no
   * record of that anywhere */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == TRUE);
  fail_unless (qcur == 2 * GST_SECOND);

  ret = gst_element_set_state (pipeline, GST_STATE_READY);
  fail_unless (ret == GST_STATE_CHANGE_SUCCESS);

  /* fails again because we are in the wrong state */
  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == FALSE);

  gst_element_set_state (pipeline, GST_STATE_NULL);

  qformat = GST_FORMAT_TIME;
  qret = gst_element_query_position (sink, &qformat, &qcur);
  fail_unless (qret == FALSE);

  gst_object_unref (sinkpad);
  gst_object_unref (pipeline);
}

GST_END_TEST;

static Suite *
fakesink_suite (void)
{
  Suite *s = suite_create ("fakesink");
  TCase *tc_chain = tcase_create ("general");

  tcase_set_timeout (tc_chain, 20);

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, test_clipping);
  tcase_add_test (tc_chain, test_preroll_sync);
  tcase_add_test (tc_chain, test_eos);
  tcase_add_test (tc_chain, test_eos2);
  tcase_add_test (tc_chain, test_position);

  return s;
}

GST_CHECK_MAIN (fakesink);

#ifndef SIGNALLING_H
#define SIGNALLING_H

#include <gst/gst.h>

void connect_to_signalling_server();
void connect_webrtc_signals(GstElement *webrtcbin);

#endif // SIGNALLING_H

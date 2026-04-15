/*
 * Copyright 2026, ablyss jb@epluribusunix.net
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include <MediaKit.h> 
#include <MediaNode.h>
#include <BufferConsumer.h>
#include <BufferProducer.h>
#include <MediaEventLooper.h>
#include <Buffer.h>
#include <MediaRoster.h> 
#include <Application.h>
#include <MediaRoster.h> 

#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <ladspa.h>
#include <SoundPlayer.h>
#include <string.h>
#include <cstdio>
#include <stdio.h>

// Types
typedef void* jack_client_t;
typedef void* jack_port_t;
typedef uint32_t jack_nframes_t;
typedef int jack_options_t;
typedef int jack_status_t;

class JaminNode : public BBufferConsumer, public BBufferProducer, public BMediaEventLooper {
public:
    JaminNode() 
    : BMediaNode("JAMin-Haiku"), 
    BBufferConsumer(B_MEDIA_RAW_AUDIO), 
    BBufferProducer(B_MEDIA_RAW_AUDIO), 
    BMediaEventLooper() 
    {
    fprintf(stderr, "[JAMin-Stubs] Node officially registered with Media Server!\n");
    fflush(stderr); 
    AddNodeKind(B_BUFFER_CONSUMER);
    AddNodeKind(B_BUFFER_PRODUCER);
    AddNodeKind(B_PHYSICAL_OUTPUT);     
    }
    
    // Standard Boilerplate
    virtual BMediaAddOn* AddOn(int32* id) const override { return NULL; }
        virtual void NodeRegistered() override {
        fprintf(stderr, "[JAMin-Stubs] Node officially registered with Media Server!\n");
        fflush(stderr);
        Run(); 
    }
    virtual status_t HandleMessage(int32 code, const void* data, size_t size) override {
        if (BBufferConsumer::HandleMessage(code, data, size) == B_OK) return B_OK;
        if (BBufferProducer::HandleMessage(code, data, size) == B_OK) return B_OK;
        if (BMediaEventLooper::HandleMessage(code, data, size) == B_OK) return B_OK;
        return BMediaNode::HandleMessage(code, data, size);
    }

    // --- CONSUMER VIRTUALS (Input Pins) ---
    virtual status_t AcceptFormat(const media_destination&, media_format* format) override { return B_OK; }
	virtual status_t GetNextInput(int32* cookie, media_input* out_input) override {
    if (*cookie != 0) return B_BAD_INDEX; // Only one input for now
    out_input->node = Node();
    out_input->destination.port = ControlPort();
    out_input->destination.id = 0;
    sprintf(out_input->name, "JAMin In");
    out_input->format.type = B_MEDIA_RAW_AUDIO;
    *cookie = 1;
    return B_OK;
	}
    virtual void DisposeInputCookie(int32) override {}
    virtual void BufferReceived(BBuffer* b) override { b->Recycle(); }
    virtual void ProducerDataStatus(const media_destination&, int32, bigtime_t) override {}
    virtual status_t GetLatencyFor(const media_destination&, bigtime_t* l, media_node_id* ts) override { 
        *l = 1000; *ts = TimeSource()->ID(); return B_OK; 
    }
    virtual status_t Connected(const media_source& s, const media_destination& d, const media_format& f, media_input* i) override {
        fprintf(stderr, "[JAMin-Stubs] Connection attempt detected in Cortex!\n");
        fflush(stderr);
        return B_OK;
    }       
    virtual void Disconnected(const media_source&, const media_destination&) override {}
    virtual status_t FormatChanged(const media_source&, const media_destination&, int32, const media_format&) override { return B_OK; }

    // --- PRODUCER VIRTUALS (Output Pins) ---
    virtual status_t FormatSuggestionRequested(media_type, int32, media_format*) override { return B_OK; }
    virtual status_t FormatProposal(const media_source&, media_format*) override { return B_OK; }
    virtual status_t FormatChangeRequested(const media_source&, const media_destination&, media_format*, int32*) override { return B_OK; }
	virtual status_t GetNextOutput(int32* cookie, media_output* out_output) override {
    if (*cookie != 0) return B_BAD_INDEX; // Only one output for now
    out_output->node = Node();
    out_output->source.port = ControlPort();
    out_output->source.id = 0;
    out_output->destination = media_destination::null;
    sprintf(out_output->name, "JAMin Out");
    out_output->format.type = B_MEDIA_RAW_AUDIO;
    *cookie = 1;
    return B_OK;
	}
    virtual status_t DisposeOutputCookie(int32) override { return B_OK; }
    virtual status_t SetBufferGroup(const media_source&, BBufferGroup*) override { return B_OK; }
    virtual status_t PrepareToConnect(const media_source&, const media_destination&, media_format*, media_source*, char*) override { return B_OK; }
    virtual void Connect(status_t, const media_source&, const media_destination&, const media_format&, char*) override {}
    virtual void Disconnect(const media_source&, const media_destination&) override {}
    virtual void LateNoticeReceived(const media_source&, bigtime_t, bigtime_t) override {}
    virtual void EnableOutput(const media_source&, bool, int32*) override {}
   
   
    // HandleEvent
    virtual void HandleEvent(const media_timed_event* event, bigtime_t lateness, bool realTimeEvent) override {
        // Minimal implementation: if it's a buffer event, handle it.
        // Otherwise, do nothing. 
        if (event->type == BTimedEventQueue::B_HANDLE_BUFFER && event->pointer != NULL) {
            BufferReceived((BBuffer*)event->pointer);
        }
    }
};


typedef struct { 
    uint32_t frame; 
    uint32_t frame_rate; 
} jack_position_t;

static int (*global_process_callback)(uint32_t, void*) = NULL;
static void* global_process_arg = NULL;
static float jamin_out_L[4096];
static float jamin_out_R[4096];

static int32 app_thread_func(void* arg) {
    be_app->Run();
    return 0;
}


// Haiku Hook: This is where the magic happens
void haiku_audio_hook(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format) {
    if (global_process_callback) {
        // Calculate frames: Haiku 'size' is in bytes. 
        // For stereo float: bytes / (2 channels * 4 bytes per float)
        uint32_t nframes = size / 8; 
        if (nframes > 4096) nframes = 4096;

        // EXECUTE JAMIN DSP
        // This fills jamin_out_L and jamin_out_R
        global_process_callback(nframes, global_process_arg);
        
        // INTERLEAVE FOR HAIKU
        // JAMin gives us Mono L and Mono R; Haiku expects [L, R, L, R...]
        float* out = (float*)buffer;
        for (uint32_t i = 0; i < nframes; i++) {
            out[i*2]     = jamin_out_L[i];
            out[i*2 + 1] = jamin_out_R[i];
        }
    } else {
        // Silence if no callback registered
        memset(buffer, 0, size);
    }
}


extern "C" {

    //  register the internal node

  void* haiku_jack_init(const char* name) {
    fprintf(stderr, "[JAMin-Stubs] jack_client_open for %s\n", name);
    fflush(stderr);

    if (be_app == NULL) {
        new BApplication("application/x-vnd.Haiku-JAMin");
        // Run the app loop in the background so it can respond to Cortex
        resume_thread(spawn_thread([](void*){ be_app->Run(); return (status_t)0; }, 
                      "app_loop", B_NORMAL_PRIORITY, NULL));
    }
    
    BMediaRoster* roster = BMediaRoster::Roster();
    JaminNode* node = new JaminNode();
    roster->RegisterNode(node);
    
    // --- THE CORTEX VISIBILITY FIX ---
    media_node timeSource;
    roster->GetTimeSource(&timeSource);
    roster->SetTimeSourceFor(node->Node().node, timeSource.node);
    roster->StartNode(node->Node(), 0); 
    // ---------------------------------
    fprintf(stderr, "[JAMin-Stubs] Node %d Registered and Started\n", node->Node().node);
    fflush(stderr);
     return (void*)1; 
  }


	// Map port names to IDs so we know which is Left and which is Right
	jack_port_t jack_port_register(jack_client_t c, const char* n, const char* t, unsigned long f, unsigned long b) {
    	static int port_count = 0;
    	port_count++;
    	// Simple logic: first registered port is L, second is R
    	return (jack_port_t)(uintptr_t)port_count;
	}

	// Hand the memory to JAMin when it asks
	void* jack_port_get_buffer(jack_port_t port, jack_nframes_t n) {
   	 if ((uintptr_t)port == 1) return jamin_out_L;
   	 if ((uintptr_t)port == 2) return jamin_out_R;
   	 return jamin_out_L; // Fallback
	}


	// Variable Definition for the l_notebook1 error
	void *l_notebook1 = NULL;

	// Connection Stubs
	int jack_disconnect(jack_client_t client, const char *a, const char *b) { return 0; }
	int jack_connect(jack_client_t client, const char *a, const char *b) { return 0; }

	// Port Stubs
	const char* jack_port_name(const jack_port_t port) { return "stub_port"; }
	const char* jack_port_short_name(const jack_port_t port) { return "stub"; }
	int jack_port_flags(const jack_port_t port) { return 0; }
	int jack_port_connected_to(const jack_port_t port, const char *port_name) { return 0; }
	int jack_port_connected(const jack_port_t port) { return 0; }


	int jack_client_close(jack_client_t client) { return 0; }
	int jack_activate(jack_client_t client) { return 0; }
	const char* jack_get_client_name(jack_client_t client) { return "JAMin-Haiku"; }
	uint32_t jack_get_sample_rate(jack_client_t client) { return 44100; }
	jack_nframes_t jack_get_buffer_size(jack_client_t client) { return 1024; }

	// Callback Stubs
	void jack_set_process_callback(jack_client_t c, int (*cb)(jack_nframes_t, void*), void* arg) {
	    global_process_callback = cb;
	    global_process_arg = arg;
	}

	void jack_on_shutdown(jack_client_t c, void (*cb)(void*), void* arg) {}

	// Transport Stubs
	int jack_transport_query(const jack_client_t client, jack_position_t *pos) {
	    if (pos) { pos->frame = 0; pos->frame_rate = 44100; }
	    return 0; 
	}
	void jack_transport_start(jack_client_t client) {}
	void jack_transport_stop(jack_client_t client) {}
	int jack_transport_locate(jack_client_t client, jack_nframes_t frame) { return 0; }

	// Misc Stubs
	float jack_cpu_load(jack_client_t client) { return 0.0f; }
	
	const char** jack_get_ports(jack_client_t c, const char* n, const char* t, unsigned long f) {
    fprintf(stderr, "[JAMin-Stubs] jack_get_ports called\n");
    fflush(stderr);
    // Allocate a list for 2 ports + NULL terminator
    const char** ports = (const char**)malloc(sizeof(char*) * 3);
    
    // We also malloc the strings themselves just to be safe
    ports[0] = strdup("system:playback_1");
    ports[1] = strdup("system:playback_2");
    ports[2] = NULL;
    
    return ports;
}

	void jack_free(void* ptr) {
    if (ptr) {
        fprintf(stderr, "[JAMin-Stubs] jack_free called on %p\n", ptr);
        fflush(stderr);
        free(ptr);
     }
	}

	// Missing IO/Port Stubs
	uint32_t jack_frame_time(jack_client_t c) { return 0; }
	void jack_port_set_latency(jack_port_t p, jack_nframes_t n) {}

	// Client/Callback Stubs
    void* jack_client_new(const char* name) {
        return haiku_jack_init(name);
    }
	    void* jack_client_open(const char* name, int opt, int* status, ...) {
        return haiku_jack_init(name);
    }

	int jack_set_xrun_callback(jack_client_t c, int (*cb)(void*), void* arg) { return 0; }
	int jack_set_buffer_size_callback(jack_client_t c, int (*cb)(jack_nframes_t, void*), void* arg) { return 0; }

	// Threading Stubs
	pthread_t jack_client_thread_id(jack_client_t c) { 
	    return pthread_self(); 
	}

	int jack_is_realtime(jack_client_t c) { return 0; }

	// Haiku/Pthread shim
	int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy) { return 0; }


	int pthread_getschedparam(pthread_t thread, int *policy, struct sched_param *param) {
	    if (policy) *policy = 0;
	    if (param) param->sched_priority = 0;
	    return 0;
	}

	int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param) {
	    return 0;
	}

}

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
#include <OS.h>
#include <syslog.h>
#include <stdio.h>
//__attribute__((visibility("default")))

/* --- 1. TYPE DEFINITIONS  --- */
typedef void* jack_client_t;
typedef void* jack_port_t;
typedef uint32_t jack_nframes_t;

typedef struct {
    char *buf;
    size_t len;
    size_t write_ptr;
    size_t read_ptr;
    size_t size;
    size_t size_mask;
    int mlocked;
} jack_ringbuffer_t;

typedef struct { 
    uint32_t frame; 
    uint32_t frame_rate; 
} jack_position_t;

/* --- 2. EXTERNALS (Link to io.c) --- */
extern "C" {
    // 1. DSP State and Variables
    extern volatile int dsp_state;
    extern size_t dsp_block_size;
    extern size_t dsp_block_bytes;
    extern int nchannels;
    extern int bchannels;
    
    // 2. Ringbuffers (Note: pointers to pointers since they are arrays of jack_ringbuffer_t*)
    extern jack_ringbuffer_t *in_rb[]; 
    extern jack_ringbuffer_t *out_rb[];

    // 3. Synchronization
    extern pthread_mutex_t lock_dsp;
    extern pthread_cond_t run_dsp;

    // 4. Core Functions
    int io_process(jack_nframes_t nframes, void *arg);

    // 5. Ringbuffer API (Declare each once)
    jack_ringbuffer_t* jack_ringbuffer_create(size_t sz);
    size_t jack_ringbuffer_write(jack_ringbuffer_t *rb, const char *src, size_t cnt);
    size_t jack_ringbuffer_read(jack_ringbuffer_t *rb, char *dest, size_t cnt);
    size_t jack_ringbuffer_read_space(const jack_ringbuffer_t *rb);
    void jack_ringbuffer_read_advance(jack_ringbuffer_t *rb, size_t cnt);    
}


/* --- 3. STATIC STORAGE --- */

static int (*global_process_callback)(jack_nframes_t, void*) = NULL;
static void* global_process_arg = NULL;

static float jamin_in_L[4096];
static float jamin_in_R[4096];
// Define this ONLY ONCE
static float jamin_out_bands[8][4096];







/* --- 4. HAIKU NODE CLASS --- */
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
    fOutput.destination = media_destination::null;
    fOutput.source.port = ControlPort();
	fOutput.source.id = 0;
	fOutput.node = Node();
	

    }
    media_output fOutput; 
	BBufferGroup* fBufferGroup = NULL;

status_t PublicSendBuffer(BBuffer* b, const media_source& source, const media_destination& destination) {
    return BBufferProducer::SendBuffer(b, source, destination);
}


    // Standard Boilerplate
    virtual BMediaAddOn* AddOn(int32* id) const override { return NULL; }


virtual void NodeRegistered() override {
    BMediaRoster* roster = BMediaRoster::Roster();
    media_node timeSourceNode;
    
    // 1. Get the system default time source
    if (roster->GetTimeSource(&timeSourceNode) == B_OK) {
        // 2. Tell the Roster to link this clock to our node.
        // This is the ONLY authorized way to set a clock.
        // The server will call your node's SetTimeSource() behind the scenes.
        roster->SetTimeSourceFor(Node().node, timeSourceNode.node);
    }

    fprintf(stderr, "[JAMin-Debug] Node Registered. Roster will sync clock.\n");
    
    // 3. Start the control loop
    Run(); 
}






    virtual status_t HandleMessage(int32 code, const void* data, size_t size) override {
        if (BBufferConsumer::HandleMessage(code, data, size) == B_OK) return B_OK;
        if (BBufferProducer::HandleMessage(code, data, size) == B_OK) return B_OK;
        if (BMediaEventLooper::HandleMessage(code, data, size) == B_OK) return B_OK;
        return BMediaNode::HandleMessage(code, data, size);
    }

    // --- CONSUMER VIRTUALS (Input Pins) ---
virtual status_t AcceptFormat(const media_destination& dest, media_format* format) override {
    if (format->type == B_MEDIA_NO_TYPE) format->type = B_MEDIA_RAW_AUDIO;
    if (format->type != B_MEDIA_RAW_AUDIO) return B_MEDIA_BAD_FORMAT;
    
    // Set our requirements
    format->u.raw_audio.frame_rate = 44100.0f;
    format->u.raw_audio.channel_count = 2;
    format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format->u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
    format->u.raw_audio.buffer_size = 4096; 
    
    return B_OK;
}





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
    

virtual void BufferReceived(BBuffer* b) override {
    // 1. Check State (3 = B_RUNNING)
    int32 state = this->RunState();
    static int state_check = 0;
    if (state_check++ % 100 == 0) {
        fprintf(stderr, "[JAMin-Debug] Node State: %s (%d)\n", 
                (state == 3) ? "RUNNING" : "STOPPED", (int)state);
    }

    if (!b || !in_rb[0] || !out_rb[0]) {
        if (b) b->Recycle();
        return;
    }

    float* incomingData = (float*)b->Data();
    size_t nFrames = b->SizeUsed() / (sizeof(float) * 2);
    size_t nBytesPerChannel = nFrames * sizeof(float);

    // 2. Feed Input
    for (size_t i = 0; i < nFrames; i++) {
        jack_ringbuffer_write(in_rb[0], (const char*)&incomingData[i * 2], sizeof(float));
        jack_ringbuffer_write(in_rb[1], (const char*)&incomingData[i * 2 + 1], sizeof(float));
    }

    // 3. Wake Engine
    pthread_mutex_lock(&lock_dsp);
    pthread_cond_signal(&run_dsp);
    pthread_mutex_unlock(&lock_dsp);

    // 4. Handle Output Buffer
    BBuffer* outBuffer = NULL;
    bool usingNewBuffer = false;
    if (fBufferGroup) {
        outBuffer = fBufferGroup->RequestBuffer(b->SizeUsed(), 0);
        if (outBuffer) usingNewBuffer = true;
    }
    if (!outBuffer) {
        outBuffer = b;
        usingNewBuffer = false;
    }

    float* outData = (float*)outBuffer->Data();

    // 5. Drain DSP and Generate Tone
    static float phase = 0.0f;
    size_t availL = jack_ringbuffer_read_space(out_rb[0]);
    size_t availR = jack_ringbuffer_read_space(out_rb[1]);

    for (size_t f = 0; f < nFrames; f++) {
        float sL = 0.0f, sR = 0.0f;
        if (availL >= sizeof(float) && availR >= sizeof(float)) {
            jack_ringbuffer_read(out_rb[0], (char*)&sL, sizeof(float));
            jack_ringbuffer_read(out_rb[1], (char*)&sR, sizeof(float));
            availL -= sizeof(float);
            availR -= sizeof(float);
        }
        float buzz = sinf(phase) * 0.1f;
        phase += 0.0157f;
        if (phase > 6.2831f) phase -= 6.2831f;

        outData[f * 2]     = sL + buzz;
        outData[f * 2 + 1] = sR + buzz;
    }

    for (int i = 2; i < bchannels; i++) {
        size_t bandAvail = jack_ringbuffer_read_space(out_rb[i]);
        if (bandAvail >= nBytesPerChannel)
            jack_ringbuffer_read_advance(out_rb[i], nBytesPerChannel);
    }

    // 6. Finalize and Send
    if (fOutput.destination != media_destination::null) {
        media_header* hdr = outBuffer->Header();
        hdr->type = B_MEDIA_RAW_AUDIO;
        hdr->size_used = outBuffer->Size();
        // FORCE the start_time to 'now'
        hdr->start_time = TimeSource()->Now(); 

        if (PublicSendBuffer(outBuffer, fOutput.source, fOutput.destination) != B_OK) {
            outBuffer->Recycle();
        }
        if (usingNewBuffer) b->Recycle();
    } else {
        outBuffer->Recycle();
        if (usingNewBuffer) b->Recycle();
    }
}













    virtual void ProducerDataStatus(const media_destination&, int32, bigtime_t) override {}
	virtual status_t GetLatencyFor(const media_destination&, bigtime_t* l, media_node_id* ts) override { 
    // Report roughly 25ms of internal processing delay
    *l = 25000; 
    *ts = TimeSource()->ID(); 
    return B_OK; 
	}

virtual status_t Connected(const media_source& s, const media_destination& d, 
                           const media_format& f, media_input* i) override {
    fprintf(stderr, "[JAMin-Stubs] Input side connected!\n");
    return B_OK;
}

virtual void Connect(status_t error, const media_source& source, 
                     const media_destination& destination, 
                     const media_format& format, char* ioName) override {
    if (error == B_OK) {
        fOutput.destination = destination;
        fOutput.source = source;
        
        // If the consumer (e.g. HD Audio) hasn't called SetBufferGroup yet,
        // we create a local one to ensure we are sending official BBuffers.
        if (fBufferGroup == NULL) {
            fprintf(stderr, "[JAMin-Debug] No group provided by consumer. Creating local group.\n");
            fBufferGroup = new BBufferGroup(8192, 8); // 8 buffers, slightly larger for safety
        }
        
        fprintf(stderr, "[JAMin-Debug] Connection LIVE to Port %d\n", (int)destination.port);
    }
}


  
    virtual void Disconnected(const media_source&, const media_destination&) override {}
    virtual status_t FormatChanged(const media_source&, const media_destination&, int32, const media_format&) override { return B_OK; }

    // --- PRODUCER VIRTUALS (Output Pins) ---
    virtual status_t FormatSuggestionRequested(media_type, int32, media_format*) override { return B_OK; }
    virtual status_t FormatProposal(const media_source&, media_format*) override { return B_OK; }
    virtual status_t FormatChangeRequested(const media_source&, const media_destination&, media_format*, int32*) override { return B_OK; }
virtual status_t GetNextOutput(int32* cookie, media_output* out_output) override {
    if (*cookie != 0) return B_BAD_INDEX;
    out_output->node = Node();
    out_output->source.port = ControlPort();
    out_output->source.id = 0;
    out_output->destination = media_destination::null; // CRITICAL: Mark as free
    sprintf(out_output->name, "JAMin Out");
    out_output->format.type = B_MEDIA_RAW_AUDIO;
    *cookie = 1;
    return B_OK;
}


    virtual status_t DisposeOutputCookie(int32) override { return B_OK; }
    
    
virtual status_t SetBufferGroup(const media_source& for_source, BBufferGroup* group) override {
    if (for_source != fOutput.source) return B_MEDIA_BAD_SOURCE;
    
    // Save the hardware's preferred buffer group
    fBufferGroup = group;
    fprintf(stderr, "[JAMin-Debug] Hardware provided a BufferGroup.\n");
    return B_OK;
}
    
    
virtual status_t PrepareToConnect(const media_source& source, const media_destination& destination, 
                                 media_format* format, media_source* out_source, char* out_name) override {
    // 1. Basic Format Validation
    if (format->type != B_MEDIA_RAW_AUDIO) return B_MEDIA_BAD_FORMAT;

    // 2. Set our Preferred Format (Matches your engine)
    format->u.raw_audio.frame_rate = 44100.0f;
    format->u.raw_audio.channel_count = 2;
    format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format->u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
    
    // Most hardware prefers a specific buffer size (usually 4096 bytes for 1024 frames)
    format->u.raw_audio.buffer_size = 4096; 

    // 3. Return our source and name
    *out_source = fOutput.source;
    strncpy(out_name, "JAMin Master Out", B_MEDIA_NAME_LENGTH);

    return B_OK;
}

virtual void Disconnect(const media_source& source, const media_destination& destination) override {
    if (source == fOutput.source) {
        fOutput.destination = media_destination::null;
        fBufferGroup = NULL;
   		 }
}
    
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


static JaminNode* g_jamin_node = NULL; // Add this line




/* --- 5. THE AUDIO HOOK --- */
void haiku_audio_hook(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format) {
    if (global_process_callback) {
        uint32_t nframes = size / (sizeof(float) * 2); // 2 channels
        if (nframes > 4096) nframes = 4096;

        float* in_out_ptr = (float*)buffer;

        // 1. De-interleave: Copy Haiku's incoming buffer to JAMin's static input arrays
        for (uint32_t i = 0; i < nframes; i++) {
            jamin_in_L[i] = in_out_ptr[i*2];
            jamin_in_R[i] = in_out_ptr[i*2 + 1];
        }

        /* 2. PROCESS & QUEUE
           io_process() will:
           - Write jamin_in_L/R into the input ringbuffers.
           - Signal the DSP thread to wake up.
           - READ the LATEST processed data from the output ringbuffers
             back into your output arrays.
        */
        io_process(nframes, NULL);

        // 3. Interleave: Copy the processed results back to the Haiku buffer
        // Note: This will be the result of the PREVIOUS dsp cycle (latency),
        // which is how realtime audio threading works.
        for (uint32_t i = 0; i < nframes; i++) {
            in_out_ptr[i*2]     = jamin_out_bands[0][i]; // Master L
            in_out_ptr[i*2 + 1] = jamin_out_bands[1][i]; // Master R
        }
    } else {
        memset(buffer, 0, size);
    }
}




void auto_connect_jamin(JaminNode* node) {
    BMediaRoster* roster = BMediaRoster::Roster();
    media_node mixer, physicalOutput;
    
    if (roster->GetAudioMixer(&mixer) != B_OK || roster->GetAudioOutput(&physicalOutput) != B_OK) return;

    media_output mixerOutput;
    media_input hardwareInput;
    int32 count = 0;

    // 1. FIND existing connection
    if (roster->GetConnectedOutputsFor(mixer, &mixerOutput, 1, &count) == B_OK && count > 0) {
        fprintf(stderr, "[JAMin-Auto] Interposing... Breaking Mixer -> Hardware link.\n");
        
        // 1. Look up the destination node using the port
        media_node destNode;
        roster->GetNodeFor(mixerOutput.destination.port, &destNode);
        
        // 2. Save the hardware destination details for our connection later
        hardwareInput.node = destNode;
        hardwareInput.destination = mixerOutput.destination;
        
        // 3. THE FIX: Use destNode.node (the ID from the object we just found)
        // instead of trying to access mixerOutput.destination.node
        roster->Disconnect(mixerOutput.node.node, mixerOutput.source, 
                          destNode.node, mixerOutput.destination);
    }



 	else {
        int32 freeCount = 0;
        roster->GetFreeOutputsFor(mixer, &mixerOutput, 1, &freeCount, B_MEDIA_RAW_AUDIO);
        roster->GetFreeInputsFor(physicalOutput, &hardwareInput, 1, &freeCount, B_MEDIA_RAW_AUDIO);
    }


    media_input jaminInput;
    media_output jaminOutput;
    int32 cookie = 0;

    // 2. Connect: MIXER -> JAMIN
    node->GetNextInput(&cookie, &jaminInput);
    status_t err1 = roster->Connect(mixerOutput.source, jaminInput.destination, 
                                   &mixerOutput.format, &mixerOutput, &jaminInput);

    // 3. Connect: JAMIN -> HARDWARE
    cookie = 0;
    node->GetNextOutput(&cookie, &jaminOutput);
    status_t err2 = roster->Connect(jaminOutput.source, hardwareInput.destination, 
                                   &jaminOutput.format, &jaminOutput, &hardwareInput);

    fprintf(stderr, "[JAMin-Auto] Connection Results: 1:%s | 2:%s\n", 
            strerror(err1), strerror(err2));
}








/* --- 6. JACK STUB IMPLEMENTATIONS --- */
extern "C" {
	
    extern volatile int dsp_state;
    extern size_t dsp_block_size;
    extern size_t dsp_block_bytes;
    extern int nchannels;
    extern int bchannels;
    


static int32 app_thread_func(void* arg) {
    be_app->Run();
    return 0;
}


int jack_activate(jack_client_t client) {
 // We are handling this manually in io.c now
 return 0; 
}


/*
void* haiku_jack_init(const char* name) {
    if (g_jamin_node != NULL) return (void*)g_jamin_node;

    fprintf(stderr, "\n--- [DEBUG] haiku_jack_init ENTERED for: %s ---\n", name);
    fflush(stderr);

    if (be_app == NULL) {
        spawn_thread([](void*){ 
            BApplication app("application/x-vnd.Haiku-JAMin");
            app.Run();
            return (status_t)0;
        }, "app_thread", B_NORMAL_PRIORITY, NULL);
        
        while (be_app == NULL) snooze(10000); 
    }

    // Create the node and save it to the static variable
    g_jamin_node = new JaminNode();
    BMediaRoster* roster = BMediaRoster::Roster();
    roster->RegisterNode(g_jamin_node);
    
    media_node timeSource;
    roster->GetTimeSource(&timeSource);
    roster->SetTimeSourceFor(g_jamin_node->Node().node, timeSource.node);

    BTimeSource* ts = roster->MakeTimeSourceFor(timeSource);
    bigtime_t now = ts->Now();
    roster->StartNode(g_jamin_node->Node(), now); 
    ts->Release();

    fprintf(stderr, "[JAMin-Stubs] Node Registered and Started at %lld\n", now);
    
    // Wire it up

    // Initialize DSP variables
    nchannels = 2;
    bchannels = 8;
    dsp_block_size = 1024;
    dsp_block_bytes = dsp_block_size * sizeof(float);

   // auto_connect_jamin(g_jamin_node);
    return (void*)g_jamin_node;   

}

	// Map port names to IDs so we know which is Left and which is Right
	jack_port_t jack_port_register(jack_client_t c, const char* n, const char* t, unsigned long f, unsigned long b) {
    	static int port_count = 0;
    	port_count++;
    	// Simple logic: first registered port is L, second is R
    	return (jack_port_t)(uintptr_t)port_count;
	}

void* jack_port_get_buffer(jack_port_t port, jack_nframes_t n) {
    uintptr_t id = (uintptr_t)port;
    
    // If it's an input port (JAMin reads from these)
    // You'll need to identify which ports are inputs in register, 
    // but usually JAMin registers inputs first.
    if (id == 1) return jamin_in_L;
    if (id == 2) return jamin_in_R;

    // Output ports (JAMin writes to these)
    // Mapping IDs 3-10 to our 8 output bands
    if (id >= 3 && id <= 10) {
        return jamin_out_bands[id - 3];
    }

    return jamin_out_bands[0]; // Fallback to Master L
}
*/

/* --- 6. JACK STUB IMPLEMENTATIONS --- */
extern "C" {
    
    // We remove the extern float* lines because they are already defined 
    // at the top of your file as: static float jamin_in_L[4096]; etc.
    
    extern volatile int dsp_state;
    extern size_t dsp_block_size;
    extern size_t dsp_block_bytes;
    extern int nchannels;
    extern int bchannels;
    void jack_ringbuffer_reset(jack_ringbuffer_t *rb) {
    if (rb == NULL) return;
    rb->read_ptr = 0;
    rb->write_ptr = 0;
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

	const char* jack_get_client_name(jack_client_t client) { return "JAMin-Haiku"; }
	uint32_t jack_get_sample_rate(jack_client_t client) { return 44100; }
	jack_nframes_t jack_get_buffer_size(jack_client_t client) { return 1024; }

	// Callback Stubs
	void jack_set_process_callback(jack_client_t c, int (*cb)(jack_nframes_t, void*), void* arg) {
	    global_process_callback = cb;
	    global_process_arg = arg;
	}    
   

    // 1. Port Registration
    jack_port_t jack_port_register(jack_client_t c, const char* n, const char* t, unsigned long f, unsigned long b) {
        static int port_count = 0;
        port_count++;
        fprintf(stderr, "[JAMin-Stub] Registering port %d: %s\n", port_count, n);
        return (jack_port_t)(uintptr_t)port_count;
    }

    // 2. Buffer Mapping - Using the existing static arrays
    void* jack_port_get_buffer(jack_port_t port, jack_nframes_t n) {
        uintptr_t id = (uintptr_t)port;
        
        // Input mapping
        if (id == 1) return (void*)jamin_in_L;
        if (id == 2) return (void*)jamin_in_R;

        // Output mapping (Bands 1-8)
        if (id >= 3 && id <= 10) {
            return (void*)jamin_out_bands[id - 3];
        }

        return (void*)jamin_out_bands[0]; 
    }
    
    
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
/*	
const char** jack_get_ports(jack_client_t c, const char* n, const char* t, unsigned long f) {
    // Static storage ensures we don't leak memory on every UI refresh
    static const char* static_ports[] = {"system:playback_1", "system:playback_2", NULL};
    
    static int port_log = 0;
    if (port_log++ % 100 == 0) {
        fprintf(stderr, "[JAMin-Stubs] jack_get_ports called (using static list)\n");
    }
    return static_ports;
}
*/

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
    int jack_set_buffer_size_callback(jack_client_t*, int (*)(uint32_t, void*), void*) { return 0; }
    int jack_set_xrun_callback(jack_client_t*, int (*)(void*), void*) { return 0; }
    void jack_on_shutdown(jack_client_t*, void (*)(void*), void*) { }

    // 3. MAIN INIT (Cortex Manual Style)
    void* haiku_jack_init(const char* name) {
        if (g_jamin_node != NULL) return (void*)g_jamin_node;

        if (be_app == NULL) {
            thread_id tid = spawn_thread([](void*) -> status_t { 
                BApplication app("application/x-vnd.Haiku-JAMin");
                app.Run();
                return B_OK;
            }, "jamin_app_thread", B_NORMAL_PRIORITY, NULL);
            resume_thread(tid);
            while (be_app == NULL) snooze(10000); 
        }

    nchannels = 2;
    bchannels = 8;
    dsp_block_size = 1024;
    dsp_block_bytes = dsp_block_size * sizeof(float);

    size_t rb_size = 65536; 
    for (int i = 0; i < bchannels; i++) {
        // IMPORTANT: Only create and reset in_rb for the 2 input channels
        if (i < nchannels) {
            in_rb[i] = jack_ringbuffer_create(rb_size);
            if (in_rb[i]) jack_ringbuffer_reset(in_rb[i]);
        } else {
            in_rb[i] = NULL; // Ensure unused inputs are null
        }
        
        // Always create 8 outputs for the crossover bands
        out_rb[i] = jack_ringbuffer_create(rb_size);
        if (out_rb[i]) jack_ringbuffer_reset(out_rb[i]);
    }

    g_jamin_node = new JaminNode();
    BMediaRoster* roster = BMediaRoster::Roster();
    roster->RegisterNode(g_jamin_node);
    
    media_node timeSource;
    roster->GetTimeSource(&timeSource);
    roster->SetTimeSourceFor(g_jamin_node->Node().node, timeSource.node);
    
    BTimeSource* ts = roster->MakeTimeSourceFor(timeSource);
    // Use a slight delay (50ms) to ensure the server is ready
    bigtime_t startTime = ts->Now() + 50000; 
    roster->StartNode(g_jamin_node->Node(), startTime);
    ts->Release();

    return (void*)g_jamin_node;   
}

    
    const char ** jack_get_ports (jack_client_t *client, const char *port_name_pattern, 
                              const char *type_name_pattern, unsigned long flags) {
    // Return a NULL-terminated array with no ports
    // This prevents the UI from trying to iterate over "phantom" JACK ports
    static const char *empty_ports[] = { NULL };
    return empty_ports;
    }
	
    void* jack_client_new(const char* name) {
        // Log to both stderr and system log for redundancy
        fprintf(stderr, "[STUB] jack_client_new: %s\n", name);
        fflush(stderr);
        syslog(LOG_ERR, "JAMIN_STUB: jack_client_new called"); 
        return haiku_jack_init(name);
    }


    void* jack_client_open(const char* name, int opt, int* status, ...) {
        fprintf(stderr, "[STUB] jack_client_open: %s\n", name);
        fflush(stderr);
        syslog(LOG_ERR, "JAMIN_STUB: jack_client_open called");
        return haiku_jack_init(name);
    }

 	// Threading Stubs
	pthread_t jack_client_thread_id(jack_client_t c) { 
	    return pthread_self(); 
	}

	int jack_is_realtime(jack_client_t c) { return 0; }

	// Haiku/Pthread shim
	int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy) { return 0; }


int pthread_getschedparam(pthread_t thread, int *policy, struct sched_param *param) {
    if (policy) *policy = 0;
    if (param) {
        // Baseline priority so JAMin's internal math stays valid
        param->sched_priority = 20; 
  	 			 }
    return 0;  
}
}
}

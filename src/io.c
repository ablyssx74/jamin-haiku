/*
 *  io.c -- JAMin I/O driver.
 *
 *  Copyright (C) 2003, 2004 Jack O'Quin.
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*  DSP Engine
 *
 *  The DSP engine is managed as if it were firmware running on a
 *  separate signal processing board.  It uses two realtime threads:
 *  the JACK thread and the DSP thread.  The JACK thread runs the JACK
 *  process() callback.  In some cases, signal processing is invoked
 *  directly from the JACK thread.  But, when the JACK period is too
 *  short for efficiently computing the FFT, signal processing should
 *  be done in the DSP thread, instead.
 *
 *  The DSP thread is created if the -t option was not specified and
 *  the process is capable of creating a realtime thread.  Otherwise,
 *  all signal processing will be done in the JACK thread, regardless
 *  of buffer size.
 *
 *  The JACK buffer size could change dynamically due to the
 *  jack_set_buffer_size_callback() function.  So, we do not assume
 *  that this buffer size is fixed.  Since current versions of JACK
 *  (May 2003) do not support that feature, there is no way to test
 *  that it is handled correctly.
 */

/*  Changes to this file should be tested for these conditions...
 *
 *  without -t option
 *	+ JACK running realtime (as root)
 *	   + JACK buffer size < DSP block size
 *	   + JACK buffer size >= DSP block size
 *	+ JACK running realtime (using capabilities)
 *	   + JACK buffer size < DSP block size
 *	   + JACK buffer size >= DSP block size
 *	+ JACK not running realtime
 *
 *  with -t option
 *	+ JACK running realtime
 *	   + JACK buffer size < DSP block size
 *	   + JACK buffer size >= DSP block size
 *	+ JACK not running realtime
 */

#include "config.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <getopt.h>
#include <errno.h>
#include <assert.h>
#include <jack/jack.h>
#ifdef HAVE_JACK_CREATE_THREAD
#include <jack/thread.h>
#endif

#include "ringbuffer.h"		/* uses <jack/ringbuffer.h>, if available */
#include "process.h"
#include "resource.h"
#include "plugin.h"
#include "io.h"
#include "transport.h"
#include "jackstatus.h"
#include "state.h"
#include "spectrum.h"
#include "preferences.h"
#include "debug.h"
#include "help.h"
#include "support.h"

/* Forward declaration of the Haiku bridge */
void* haiku_jack_init(const char* name);

char *jamin_options = "dFf:j:n:hprTtvVl:s:c:igD";   /* valid JAMin options */
char *pname;				      /* `basename $0` */
int dummy_mode = 0;			      /* -d option */
int all_errors_fatal = 0;		      /* -F option */
int show_help = 0;			      /* -h option */
int connect_ports = 1;			      /* -p option */
int trace_option = 0;			      /* -T option */
int thread_option = 1;			      /* -t option */
int debug_level = DBG_OFF;		      /* -v option */
//int debug_level = DBG_VERBOSE;
char session_file[PATH_MAX];		      /* -f option */
int gui_mode = 0;			      /* -g/-D option : Classic, Presets, Daemon*/
int limiter_plugin_type;                      /* -l option - 0=Steve's fast, 1=Sampo's foo */
static char *errstr;


/*  Synchronization within the DSP engine is managed as a finite state
 *  machine.  These state transitions are the key to understanding
 *  this component.
 */
#define DSP_INIT	001
#define DSP_ACTIVATING	002
#define DSP_STARTING	004
#define DSP_RUNNING	010
#define DSP_STOPPING	020
#define DSP_STOPPED	040

#define DSP_STATE_IS(x)		((dsp_state)&(x))
#define DSP_STATE_NOT(x)	((dsp_state)&(~(x)))
volatile int dsp_state = DSP_INIT; 


int have_dsp_thread = 0;		/* DSP thread exists? */
size_t dsp_block_size = 1024;
size_t dsp_block_bytes;		/* DSP chunk size in bytes */

#define DSP_PRIORITY_DIFF 1	/* DSP thread priority difference */
pthread_t dsp_thread;	/* DSP thread handle */
pthread_cond_t run_dsp = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lock_dsp = PTHREAD_MUTEX_INITIALIZER;

#define NCHUNKS 4		/* number of DSP blocks in ringbuffer */
jack_ringbuffer_t *in_rb[NCHANNELS];  /* input channel buffers */
jack_ringbuffer_t *out_rb[BCHANNELS]; /* output channel buffers */

/* JACK connection data */
io_jack_status_t jst = {0};		/* current JACK status */
jack_client_t *client;			/* JACK client structure */
char *client_name = NULL;		/* JACK client name (in heap) */
char *server_name = NULL;		/* JACK server name (in heap) */
int nchannels = NCHANNELS;		/* actual number of channels */
int bchannels = BCHANNELS;  /* actual numbers of xover channels */

/* These arrays are NULL-terminated... */
jack_port_t *input_ports[NCHANNELS+1] = {NULL};
jack_port_t *output_ports[BCHANNELS+1] = {NULL};

static const char *in_names[NCHANNELS] = {"in_L", "in_R"};
static const char *out_names[BCHANNELS] = {"a.master.out_L", "a.master.out_R","b.low.out_L", "b.low.out_R", "c.mid.out_L", "c.mid.out_R", "d.high.out_L", "d.high.out_R" };
static const char *iports[NCHANNELS] = {NULL, NULL};
static const char *oports[BCHANNELS] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};


pthread_mutex_t io_trace_lock = PTHREAD_MUTEX_INITIALIZER;
#define TR_BUFSIZE	256		/* must be power of 2 */
#define TR_MSGSIZE	60
struct io_trace_t {
    jack_nframes_t timestamp;		/* JACK timestamp */
    char message[TR_MSGSIZE];		/* trace message */
};
size_t tr_next = 0;			/* next tr_buf entry */
struct io_trace_t tr_buf[TR_BUFSIZE] = {{0}};

void io_trace(const char *fmt, ...)
{
    va_list ap;

    /* if lock already held, skip this entry */
    if (pthread_mutex_trylock(&io_trace_lock) == 0) {

	/* get frame time from JACK, if it is active. */
	if (client)
	    tr_buf[tr_next].timestamp = jack_frame_time(client);
	else
	    tr_buf[tr_next].timestamp = 0;

	/* format trace message */
	va_start(ap, fmt);
	vsnprintf(tr_buf[tr_next].message, TR_MSGSIZE, fmt, ap);
	va_end(ap);

	tr_next = (tr_next+1) & (TR_BUFSIZE-1);

	pthread_mutex_unlock(&io_trace_lock);
    }
}


void io_list_trace()
{
    size_t t;

    pthread_mutex_lock(&io_trace_lock);

    t = tr_next;
    do {
	if (tr_buf[t].message[0] != '\0')
	    fprintf(stderr, "%s trace [%" PRIu32 "]: %s\n", PACKAGE,
		    tr_buf[t].timestamp, tr_buf[t].message);
	t = (t+1) & (TR_BUFSIZE-1);
    } while (t != tr_next);

    pthread_mutex_unlock(&io_trace_lock);
}



void io_errlog(int err, char *fmt, ...)
{
    va_list ap;
    char buffer[300];

    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    IF_DEBUG(DBG_TERSE,
	     io_trace("error %d: %s", err, buffer));
    g_print(_("%s internal error %d: %s\n"), PACKAGE, err, buffer);
    if (all_errors_fatal) {
	g_print(_(" Terminating due to -F option.\n"));
	abort();
    }
}


void io_new_state(int next)
{
    /* These transitions don't happen all that often, and they are
     * important.  So, make sure this one is valid */
    switch (next) {
    case DSP_INIT:
	goto invalid;
    case DSP_ACTIVATING:
	if (DSP_STATE_NOT(DSP_INIT))
	    goto invalid;
	break;
    case DSP_STARTING:
	if (DSP_STATE_NOT(DSP_ACTIVATING))
	    goto invalid;
	break;
    case DSP_RUNNING:
	if (DSP_STATE_NOT(DSP_ACTIVATING|DSP_STARTING))
	    goto invalid;
	break;
    case DSP_STOPPING:
	if (DSP_STATE_NOT(DSP_ACTIVATING|DSP_RUNNING|DSP_STARTING))
	    goto invalid;
	break;
    case DSP_STOPPED:
	if (DSP_STATE_NOT(DSP_INIT|DSP_STOPPING))
	    goto invalid;
	break;
    default:
    invalid:
	io_errlog(EDEADLK, "invalid DSP state transition: 0%o -> 0%o.",
		  dsp_state, next);
	return;				/* don't do it */
    }
    dsp_state = next;			/* change to new state */
    IF_DEBUG(DBG_TERSE, io_trace("new DSP state: 0%o.", next));
}


/* io_get_status -- collect current JACK status. */
void io_get_status(io_jack_status_t *jp)
{
    if (client)
	jst.cpu_load = jack_cpu_load(client);
    *jp = jst;
}


/* io_set_latency -- set DSP engine latencies. */
void io_set_latency(int source, jack_nframes_t delay)
{
    static jack_nframes_t latency_delay[LAT_NSOURCES] = {0};
    static char *latency_sources[LAT_NSOURCES] = {
	"I/O Buffering",
	"Fourier Transform",
	"Limiter"};
    int chan;

    if (source < 0 || source >= LAT_NSOURCES) {
	io_errlog(ENOENT, "unknown latency source: %d.", source);
	return;
    }

    IF_DEBUG(DBG_TERSE,
	     io_trace("latency due to %s is %ld frames.",
		      latency_sources[source], delay));
    jst.latency += delay - latency_delay[source];
    latency_delay[source] = delay;

    /* Set JACK port latencies (after ports connected). */
    if (DSP_STATE_NOT(DSP_INIT|DSP_STOPPED))
	for (chan = 0; chan < bchannels; chan++) {
	    jack_port_set_latency(output_ports[chan], jst.latency);
	}
}



int ooooooooooooooooooooooooooio_get_dsp_buffers(int nchannels, int bchannels,
		       jack_default_audio_sample_t *in[NCHANNELS],
		       jack_default_audio_sample_t *out[BCHANNELS])
{
    int chan;
    jack_ringbuffer_data_t io_vec[2];

    for (chan = 0; chan < bchannels; chan++) {
		if( chan < nchannels ){
			if (jack_ringbuffer_read_space(in_rb[chan]) < dsp_block_bytes)
				return 0;			/* not enough space */
		}		
		if (jack_ringbuffer_write_space(out_rb[chan]) < dsp_block_bytes)
			return 0;			/* not enough space */

	/* Copy buffer pointers to in[] and out[].  If the ringbuffer
	 * space was discontiguous, we either need to rebuffer or
	 * extend the interface to process_signal() to allow this
	 * situation.  But, that's not implemented yet, hence the
	 * asserts. */
		if( chan < nchannels ){
			jack_ringbuffer_get_read_vector(in_rb[chan], io_vec);
			in[chan] = (jack_default_audio_sample_t *) io_vec[0].buf;
			assert(io_vec[0].len >= dsp_block_bytes); /* must be contiguous */
		}
			
		jack_ringbuffer_get_write_vector(out_rb[chan], io_vec);
		out[chan] = (jack_default_audio_sample_t *) io_vec[0].buf;
		assert(io_vec[0].len >= dsp_block_bytes); /* must be contiguous */
    }
    return 1;				/* success */
}

int io_get_dsp_buffers(int nchannels, int bchannels,
                       jack_default_audio_sample_t *in[NCHANNELS],
                       jack_default_audio_sample_t *out[BCHANNELS])
{
    int chan;
    jack_ringbuffer_data_t io_vec[2];

    for (chan = 0; chan < bchannels; chan++) {
        /* 1. Input Check: Wait until there is a full block to read */
        if (chan < nchannels) {
            if (jack_ringbuffer_read_space(in_rb[chan]) < dsp_block_bytes)
                return 0;			
        }		
		
        /* 2. Output Check: Wait until there is space to write the result.
           Your BufferReceived call to jack_ringbuffer_read will clear this space. */

		if (jack_ringbuffer_write_space(out_rb[chan]) < dsp_block_bytes) {
    		static int blocked_chan = -1;
    		if (blocked_chan != chan) {
        		fprintf(stderr, "[JAMin-DSP] Blocked by Output Channel: %d\n", chan);
        		blocked_chan = chan;
   		 }
    		return 0;
		}


        /* 3. Setup Pointers: Use the non-copying vector API for speed */
        if (chan < nchannels) {
            jack_ringbuffer_get_read_vector(in_rb[chan], io_vec);
            in[chan] = (jack_default_audio_sample_t *) io_vec[0].buf;
            // Ensure the ringbuffer didn't wrap in the middle of our block
            assert(io_vec[0].len >= dsp_block_bytes); 
        }
			
        jack_ringbuffer_get_write_vector(out_rb[chan], io_vec);
        out[chan] = (jack_default_audio_sample_t *) io_vec[0].buf;
        assert(io_vec[0].len >= dsp_block_bytes); 
    }
    return 1;
}




/* io_dsp_thread -- DSP thread main loop. */
void *io_dsp_thread(void *arg)
{
    jack_default_audio_sample_t *in[NCHANNELS], *out[BCHANNELS];
    int chan;
    int rc;
    static int loop_count = 0;

    fprintf(stderr, "[JAMin-DSP] Thread started.\n");

    pthread_mutex_lock(&lock_dsp);

    // Initial state transition
    if (DSP_STATE_IS(DSP_ACTIVATING | DSP_STARTING | DSP_INIT)) {
        dsp_state = DSP_RUNNING;
        fprintf(stderr, "[JAMin-DSP] Force-switched to RUNNING (State: %d)\n", dsp_state);
    }

    // Use the defined state check for the loop
    while (DSP_STATE_NOT(DSP_STOPPING)) {
        
        // --- THE WORKHORSE ---
        if (io_get_dsp_buffers(nchannels, bchannels, in, out)) {
            do {
                rc = process_signal(dsp_block_size, nchannels, bchannels, in, out);
                
                if (loop_count++ % 500 == 0) {
                    fprintf(stderr, "[JAMin-DSP] Processing... State: %d | Loop: %d\n", dsp_state, loop_count);
                }

                if (rc != 0)
                    io_errlog(EAGAIN, "signal processing error: %d.", rc);

                for (chan = 0; chan < bchannels; chan++) {
                    jack_ringbuffer_write_advance(out_rb[chan], dsp_block_bytes);
                    if (chan < nchannels) {	
                        jack_ringbuffer_read_advance(in_rb[chan], dsp_block_bytes);
                    }
                }
            } while (io_get_dsp_buffers(nchannels, bchannels, in, out));
        } else {
            // DIAGNOSTIC: If we wake up but don't process, let's check why.
            static int idle_log = 0;
            if (idle_log++ % 500 == 0) {
                // If in_rb[0] is valid, check how many bytes are actually in it
                size_t fill = (in_rb[0]) ? jack_ringbuffer_read_space(in_rb[0]) : 0;
                fprintf(stderr, "[JAMin-DSP] Idle wakeup. RB Fill: %zu, Need: %zu\n", fill, dsp_block_bytes);
            }
        }

        // Wait for BufferReceived to signal us
        rc = pthread_cond_wait(&run_dsp, &lock_dsp);
        
        if (rc != 0) {
            fprintf(stderr, "[JAMin-DSP] pthread_cond_wait error: %d\n", rc);
        }
    };

    pthread_mutex_unlock(&lock_dsp);
    fprintf(stderr, "[JAMin-DSP] Thread exiting.\n");
    return NULL;
}



void io_schedule()
{
    // On Haiku, we want to ensure the signal is sent.
    // If the DSP thread is busy, the signal will be caught 
    // immediately after it hits pthread_cond_wait.
    pthread_mutex_lock(&lock_dsp);
    
    IF_DEBUG(DBG_NORMAL, io_trace(" DSP scheduled"));
    
    // Wake up the io_dsp_thread
    pthread_cond_signal(&run_dsp);
    
    pthread_mutex_unlock(&lock_dsp);
    
    // Optional: add a tiny debug log to confirm the stub is calling this
    static int sched_count = 0;
    if (sched_count++ % 1000 == 0) {
        fprintf(stderr, "[JAMin-IO] io_schedule: Signal sent to DSP thread.\n");
    }
}



int io_queue(jack_nframes_t nframes, int nchannels, int bchannels,
	     jack_default_audio_sample_t *in[NCHANNELS],
	     jack_default_audio_sample_t *out[BCHANNELS])
{
    int chan;
    int rc = 0;
    size_t nbytes = nframes * sizeof(jack_default_audio_sample_t);
    size_t count;

    // Use decimal 8 (DSP_RUNNING) or octal 010
    if (DSP_STATE_IS(DSP_ACTIVATING)) {
        return EBUSY;
    }

    /* 1. Queue Input Data */
    for (chan = 0; chan < nchannels; chan++) {
        if (!in_rb[chan]) continue; // Safety for Haiku port
        
        count = jack_ringbuffer_write(in_rb[chan], (const char *) in[chan], nbytes);
        
        if (count != nbytes) {
            // Under Haiku, we prefer to log and drop rather than abort()
            static int overflow_count = 0;
            if (overflow_count++ % 100 == 0) {
                fprintf(stderr, "[JAMin-IO] Input Overflow! Channel %d\n", chan);
            }
            rc = ENOSPC;
        }
    } 

    /* 2. Check if we should wake the DSP thread */
    // Only signal if we have a full block ready to crunch
    if (in_rb[0] && jack_ringbuffer_read_space(in_rb[0]) >= dsp_block_bytes) {
        io_schedule(); // This calls pthread_cond_signal(&run_dsp)
    } else {
        // DEBUG PROBE: If you aren't seeing processing, this will tell you why
        static int debug_tick = 0;
        if (debug_tick++ % 1000 == 0) {
            size_t space = in_rb[0] ? jack_ringbuffer_read_space(in_rb[0]) : 0;
            fprintf(stderr, "[JAMin-IO] Not enough data to schedule: %zu/%zu bytes\n", 
                    space, dsp_block_bytes);
        }
    }

    /* 3. Dequeue Processed Output Data */
    for (chan = 0; chan < bchannels; chan++) {
        if (!out_rb[chan]) {
            memset(out[chan], 0, nbytes);
            continue;
        }

        count = jack_ringbuffer_read(out_rb[chan], (char *) out[chan], nbytes);
        
        if (count < nbytes) {
            /* If the DSP thread hasn't finished a block yet, we fill with silence */
            void *addr = ((char *) out[chan]) + count;
            memset(addr, 0, nbytes - count);

            // Only report underflow if we are supposed to be running
            if (DSP_STATE_IS(DSP_RUNNING)) {
                static int underflow_count = 0;
                if (underflow_count++ % 1000 == 0) {
                    fprintf(stderr, "[JAMin-IO] Output Underflow (DSP too slow or not started)\n");
                }
                rc = EPIPE;
            }
        }
    }

    return rc;
}


int io_process(jack_nframes_t nframes, void *arg)
{
    jack_default_audio_sample_t *in[NCHANNELS], *out[BCHANNELS];
    int chan;
    int return_code = 0;
    int rc;

    // 1. Get the buffer pointers from your stubs
    // In your jack_stubs.cpp, jack_port_get_buffer must return valid memory
    for (chan = 0; chan < bchannels; chan++) {
        if (chan < nchannels) {
            in[chan] = (jack_default_audio_sample_t *)jack_port_get_buffer(input_ports[chan], nframes);
        } 
        out[chan] = (jack_default_audio_sample_t *)jack_port_get_buffer(output_ports[chan], nframes);
    }

    // 2. Decide: Process immediately or Queue it?
    // On Haiku, we almost ALWAYS want to use the DSP thread (io_queue) 
    // to keep the Media Server thread responsive.
    
    if (have_dsp_thread) {
        // This puts data into ringbuffers and signals io_dsp_thread
        return_code = io_queue(nframes, nchannels, bchannels, in, out);
    } else {
        // Fallback: synchronous processing (only if thread failed to start)
        if (nframes >= dsp_block_size) {
            while (nframes >= dsp_block_size) {
                rc = process_signal(dsp_block_size, nchannels, bchannels, in, out);
                if (rc != 0) return_code = rc;

                for (chan = 0; chan < bchannels; chan++) {
                    if (chan < nchannels) in[chan] += dsp_block_size;
                    out[chan] += dsp_block_size;
                }
                nframes -= dsp_block_size;
            }
        } else {
            // Very small buffer fallback
            return_code = process_signal(nframes, nchannels, bchannels, in, out);
        }
    }

    return return_code;
}


int io_xrun(void *arg)
{
    // This is called when audio "glitches" (buffer late). 
    // On Haiku, you can trigger this if your Media Node's 
    // LateNoticeReceived is called.
    ++jst.xruns;			
    
    static int xrun_count = 0;
    if (xrun_count++ % 10 == 0) {
        fprintf(stderr, "[JAMin-IO] XRUN detected! Total: %d\n", jst.xruns);
    }
    
    IF_DEBUG(DBG_TERSE, io_trace("I/O xrun"));
    return 0;
}

int io_bufsize(jack_nframes_t nframes, void *arg)
{
    // Important: JAMin uses this to calculate latency.
    // In your stub, call this when the Media Kit confirms a buffer size.
    jst.buf_size = nframes;
    
    fprintf(stderr, "[JAMin-IO] Buffer size changed to %u frames\n", nframes);
    
    // This math ensures the UI displays the correct latency.
    io_set_latency(LAT_BUFFERS,
		   (have_dsp_thread &&
		    (dsp_block_size > nframes)? dsp_block_size: 0));
    return 0;
}

// No changes needed here, standard cleanup utility.
static inline void io_free_heap(char **p)
{
    if (*p) {				
	free(*p);
	*p = NULL;			
    }
}


void io_cleanup()
{
    int chan;
    fprintf(stderr, "[JAMin-Stubs] io_cleanup called. Shutting down...\n");

    switch (dsp_state) {
        case DSP_INIT:
            dsp_state = DSP_STOPPED;
            break;

        case DSP_ACTIVATING:
        case DSP_STARTING:
        case DSP_RUNNING:
            if (have_dsp_thread) {
                pthread_mutex_lock(&lock_dsp);
                dsp_state = DSP_STOPPING; // Stop the DSP thread
                pthread_cond_signal(&run_dsp);
                pthread_mutex_unlock(&lock_dsp);
                
                fprintf(stderr, "[JAMin-Stubs] Waiting for DSP thread to join...\n");
                pthread_join(dsp_thread, NULL);
            } else {
                dsp_state = DSP_STOPPING;
            }
            break;
    };	

    if (dsp_state == DSP_STOPPING) {
        // 1. Stub Safety: Don't call real JACK functions on your fake client
        // jack_client_close(client); // REMOVE OR STUB THIS
        client = NULL;
        jst.active = 0;
        dsp_state = DSP_STOPPED;

        io_free_heap(&client_name);
        io_free_heap(&server_name);

        // 2. Free the ring buffers we allocated in haiku_jack_init
        fprintf(stderr, "[JAMin-Stubs] Freeing ringbuffers...\n");
        for (chan = 0; chan < bchannels; chan++) {
            if (chan < nchannels && in_rb[chan]) {
                jack_ringbuffer_free(in_rb[chan]);
                in_rb[chan] = NULL;
            }
            if (out_rb[chan]) {
                jack_ringbuffer_free(out_rb[chan]);
                out_rb[chan] = NULL;
            }
        }
    }

    // 3. Haiku Specific: You should tell the Media Roster to stop your node here
    // BMediaRoster::Roster()->StopNode(your_node_id, 0, true);

    fprintf(stderr, "[JAMin-Stubs] Cleanup complete.\n");
}


void io_shutdown(void *arg)
{
    // On Haiku, this might be triggered if the Media Server 
    // crashes or the user removes the node in Cortex.
    jst.active = 0;
    io_cleanup();
}




gboolean check_file (char *optarg)
{
  FILE *fp;

  if ((fp = fopen (optarg, "r")) == NULL)
    {
      // Haiku uses standard POSIX paths, so fopen should work fine.
      errstr = g_strdup_printf(_("File %s : %s\nUsing default."), optarg, 
                                strerror (errno));
      g_print("%s\n", errstr);
      
      // WARNING: If the GUI isn't fully initialized, this 'message' 
      // call (which is a GTK dialog) might hang or crash.
      message (GTK_MESSAGE_ERROR, errstr);
      
      g_free (errstr); // Use g_free for g_strdup_printf memory
      return (FALSE);
    }

  fclose (fp);
  return (TRUE);
}


jack_client_t *io_jack_open()
{
    fprintf(stderr, "[JAMin-IO] Intercepting jack_open for Haiku Media Kit...\n");

    /* 
       1. Call your Haiku initialization stub.
       This starts the BApplication, creates the JaminNode, 
       and registers it with the Media Roster/Cortex.
    */
    client = (jack_client_t*)haiku_jack_init(client_name);

    
    if (client == NULL) {
        fprintf(stderr, "[JAMin-IO] Haiku Media Kit initialization failed!\n");
        return NULL;
    }

    // 2. Set the global status to "active" so JAMin's UI knows we are running
    jst.active = 1;
    jst.buf_size = 1024; // Typical Haiku default, will be updated by io_bufsize later

    fprintf(stderr, "[JAMin-IO] Haiku Bridge Active. Node ready for Cortex.\n");

    /* 
       3. Return your "fake" client pointer. 
       (Since haiku_jack_init returns (void*)1, this satisfies JAMin's null checks)
    */
    return client;
}



void io_init(int argc, char *argv[])
{
    int chan;
    int opt, spectrum_freq;
    float crossfade_time;

    spectrum_freq = 10;
    crossfade_time = 1.0;
    gui_mode = 0;

  /* basename $0 */
    pname = strrchr(argv[0], '/');
    if (pname == 0)
	pname = argv[0];
    else
	pname++;

    while ((opt = getopt(argc, argv, jamin_options)) != -1) {
	switch (opt) {
	case 'd':			/* dummy mode, no JACK */
	    dummy_mode = 1;
	    break;
	case 'F':			/* all errors fatal */
	    all_errors_fatal = 1;
	    break;
	case 'f':
            if (check_file(optarg)) {
		strncpy(session_file, optarg, sizeof(session_file));
		s_set_session_filename (session_file);
	    }
            break;
	case 'j':			/* Set JACK server name */
	    server_name = strdup(optarg);
	    break;
	case 'n':			/* Set JACK client name */
	    client_name = strdup(optarg);
	    break;
	case 's':			/* Set spectrum update frequency */
	    sscanf (optarg, "%d", &spectrum_freq);
            if (spectrum_freq < 0 || spectrum_freq > 10) spectrum_freq = 10;
	    break;
	case 'c':			/* Set crossfade time */
	    sscanf (optarg, "%f", &crossfade_time);
            if (crossfade_time < 0.0 || crossfade_time > 2.0) 
              crossfade_time = 1.0;
	    break;
	case 'h':			/* show help */
	    show_help = 1;
	    break;
	case 'p':			/* no port connections */
	    connect_ports = 0;
	    break;
	case 'r':			/* default GTK resources */
            resource_file_name(NULL);
	    break;
	case 't':			/* no DSP thread */
	    thread_option = 0;
	    break;
	case 'T':			/* list trace output */
	    trace_option = 1;
	    break;
	case 'i':			/* Use IIR type crossover */
            process_set_crossover_type (IIR);
	    break;
	case 'g':			/* Choose which interface to display */
		gui_mode = 1;   
		//g_print(_("gui_mode = %i\n"), gui_mode);
		break;	
	case 'D':			/* Choose which interface to display */
		gui_mode = 2;   
		//g_print(_("gui_mode = %i\n"), gui_mode);
		break;			
	case 'l':			/* Select limiter, 0=Steve's fast, 1=Sampo's foo */
	    sscanf (optarg, "%d", &limiter_plugin_type);
            if (limiter_plugin_type < 0 || limiter_plugin_type > 1) limiter_plugin_type = 0;
            process_set_limiter_plugin (limiter_plugin_type);
            s_set_override_limiter_default ();
	    break;
	case 'v':			/* verbose */
	    debug_level += 1;		/* increment output level */
	    break;
	case 'V':			/* version */
	    /* version info already printed */
	    exit(9);
	default:
	    show_help = 1;
	    break;
	}
    }


    set_spectrum_freq (spectrum_freq);
    s_set_crossfade_time (crossfade_time);


    if (connect_ports) {

	/* check for input and output port names of each channel */
	if ((argc - optind) >= nchannels)
	    for (chan = 0; chan < nchannels; chan++)
		iports[chan] = argv[optind++];

	if ((argc - optind) >= nchannels)
	    for (chan = 0; chan < nchannels; chan++)
		oports[chan] = argv[optind++];
    }

    if (argc != optind)			/* any extra options? */
	show_help = 1;

    if (show_help) {
	g_print(_(
                "Usage: %s [-%s] [inport1 inport2 [outport1 outport2]]\n"
                "\nuser options:\n"
                "\t-f file\tload session file on startup\n"
                "\t-h\tshow this help\n"
                "\t-j name\tJACK server name\n"
                "\t-n name\tJACK client name\n"
                "\t-s freq\tset spectrum update frequency\n"
                "\t-c time\tcrossfade time\n"
                "\t-r\tuse example GTK resource file\n"
                "\t-p\tdo not automatically connect JACK output ports\n"
                "\t-i\tUse IIR crossover instead of FFT\n"
                "\t-l limiter\tUse fast-lookahead limiter(0) or foo-limiter(1)\n"
                "\t-v\tverbose output (use -vv... for more detail)\n"
                "\t-V\tprint JAMin version and quit\n"
                "\ndeveloper options:\n"
                "\t-d\tdummy mode (don't connect to JACK)\n"
                "\t-F\ttreat all errors as fatal\n"
                "\t-T\tprint trace buffer\n"
                "\t-t\tdon't start separate DSP thread\n"
				"\t-g\tDisplay Presets gui at startup\n"
				"\t-D\tRun in Daemon mode\n"
                "\n"),
		pname, jamin_options);
	exit(1);
    }

   

    if (dummy_mode) {
        dsp_state = DSP_STOPPED; // Removed io_new_state static dependency
        io_bufsize(1024, NULL);
        jst.sample_rate = 48000;
        process_init(48000.0f);
        return;
    }

    /* 1. Register as a Haiku Media Node (via your bridge) */
    if (!client_name) {
        client_name = strdup(PACKAGE);
    }


/* In io.c, after haiku_jack_init is called */
client = haiku_jack_init("jamin");

if (client) {
    // Manually force the state machine to RUNNING
    // This bypasses the need for a real jack_activate() call
    dsp_state = DSP_RUNNING; 
    have_dsp_thread = 1;  // Tell io_process to use the ringbuffers
    
    fprintf(stderr, "[JAMin-IO] Haiku Bridge Active. Node ready for Cortex.\n");
}


    /* 
       This calls your haiku_jack_init. 
       It MUST happen before we try to get the sample rate or buffer size.
    */
    client = io_jack_open();
    if (client == NULL) {
        fprintf(stderr, "[JAMin-IO] Failed to initialize Haiku Media Bridge\n");
        exit(2);
    }

    /* 
       2. Stub out JACK callbacks.
       In your port, these functions (jack_set_...) should be empty stubs 
       in jack_stubs.cpp because your Media Node handles the callbacks now.
    */
    jack_set_process_callback(client, io_process, NULL);
    jack_on_shutdown(client, io_shutdown, NULL);
    jack_set_xrun_callback(client, io_xrun, NULL);
    jack_set_buffer_size_callback(client, io_bufsize, NULL);

    /* 
       3. Initialize DSP Engine parameters.
       We manually set these because we aren't getting them from a JACK server.
    */
    if (dsp_block_size == 0) dsp_block_size = 1024; 
    dsp_block_bytes = dsp_block_size * sizeof(jack_default_audio_sample_t);
    
    // Set a default sample rate for Haiku (usually 44100 or 48000)
    jst.sample_rate = 44100; 

    /* 
       4. Kickstart process_signal.
       This allocates the FFT buffers and crossover filters.
    */
    process_init((float) jst.sample_rate);
    
    fprintf(stderr, "[JAMin-IO] Engine init complete. SR: %d, Block: %zu\n", 
            jst.sample_rate, dsp_block_size);
   
}




#include <OS.h> // For Haiku priority constants

int io_create_dsp_thread()
{
	set_thread_priority(find_thread(NULL), B_URGENT_DISPLAY_PRIORITY + 10);
    int rc;
    
    fprintf(stderr, "[JAMin-IO] Creating DSP thread for Haiku...\n");

    /* 1. Create the thread simply using pthread */
    rc = pthread_create(&dsp_thread, NULL, io_dsp_thread, NULL);
    
    if (rc != 0) {
        fprintf(stderr, "[JAMin-IO] Error creating DSP thread: %d\n", rc);
        return rc;
    }

    /* 
       2. Set Haiku-specific Realtime Priority.
       Since we aren't using JACK's priority inheritance, we manually
       set this thread to a high audio priority so it doesn't stutter.
    */
    thread_id haiku_tid = find_thread(NULL); // This logic needs to be inside the thread usually, 
                                             // or use the pthread_get_thread_id_np if available.
    
    // Alternative: We can set the priority from WITHIN io_dsp_thread 
    // at the very beginning of that function:
    // set_thread_priority(find_thread(NULL), B_REAL_TIME_DISPLAY_PRIORITY);

    jst.realtime = 1; 
    have_dsp_thread = 1;

    fprintf(stderr, "[JAMin-IO] DSP thread created successfully.\n");
    return 0;
}



void io_activate()
{
    int chan;
    size_t bufsize;

    if (dsp_state == DSP_STOPPED)
        return;

    dsp_state = DSP_ACTIVATING;
    fprintf(stderr, "[JAMin-IO] Activating Haiku Engine...\n");

    /* 1. Register Ports via your stubs */
    for (chan = 0; chan < nchannels; chan++) {
        input_ports[chan] = jack_port_register(client, in_names[chan], NULL, 0, 0);
    }
 
    for (chan = 0; chan < bchannels; chan++) {
        output_ports[chan] = jack_port_register(client, out_names[chan], NULL, 0, 0);
    }   
    
   /* 4. Allocate DSP engine ringbuffers. */
    // Ensure NCHUNKS is defined (usually 4)
    bufsize = dsp_block_bytes * 4; 
    
    for (chan = 0; chan < bchannels; chan++) {
        if (chan < nchannels) {
            in_rb[chan] = jack_ringbuffer_create(bufsize);
            if (in_rb[chan]) memset(in_rb[chan]->buf, 0, bufsize);
        }	
        out_rb[chan] = jack_ringbuffer_create(bufsize);
        if (out_rb[chan]) memset(out_rb[chan]->buf, 0, bufsize);
    }

    /* 2. Stub out jack_activate */
    // In your jack_stubs.cpp, make this return 0
    jack_activate(client);

    jst.active = 1;

    /* 3. Skip JACK auto-connect logic. 
       On Haiku, the user connects pins visually in Cortex. 
    */



    /* 5. Create DSP thread */
    pthread_mutex_lock(&lock_dsp);
    if (thread_option) {
        have_dsp_thread = (io_create_dsp_thread() == 0);
    } else {
        have_dsp_thread = 0;
    }

    // If thread started, io_dsp_thread will move state to RUNNING.
    // If not, we move it manually here.
    if (!have_dsp_thread) {
        dsp_state = DSP_RUNNING;
    }

    /* 6. Set Latency for the UI */
    io_set_latency(LAT_BUFFERS,
                   (have_dsp_thread && (dsp_block_size > jst.buf_size) ? dsp_block_size : 0));
    
    pthread_mutex_unlock(&lock_dsp);
    
    fprintf(stderr, "[JAMin-IO] Activation complete. State: %d\n", dsp_state);
}


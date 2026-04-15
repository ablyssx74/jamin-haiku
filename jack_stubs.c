/*
 * Copyright 2026, ablyss jb@epluribusunix.net
 * All rights reserved. Distributed under the terms of the MIT license.
 */


#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
// Add these to the top of jack_stubs.c
#include <ladspa.h>

// Types
typedef void* jack_client_t;
typedef void* jack_port_t;
typedef uint32_t jack_nframes_t;
typedef int jack_options_t;
typedef int jack_status_t;

typedef struct { 
    uint32_t frame; 
    uint32_t frame_rate; 
} jack_position_t;

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
void* jack_port_get_buffer(jack_port_t port, jack_nframes_t n) { return NULL; }

// Client Stubs
jack_client_t jack_client_open(const char* name, jack_options_t opt, jack_status_t* status, ...) { return (void*)1; }
int jack_client_close(jack_client_t client) { return 0; }
int jack_activate(jack_client_t client) { return 0; }
const char* jack_get_client_name(jack_client_t client) { return "JAMin-Haiku"; }
uint32_t jack_get_sample_rate(jack_client_t client) { return 44100; }
jack_nframes_t jack_get_buffer_size(jack_client_t client) { return 1024; }

// Callback Stubs
void jack_set_process_callback(jack_client_t c, int (*cb)(jack_nframes_t, void*), void* arg) {}
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
const char** jack_get_ports(jack_client_t c, const char* n, const char* t, unsigned long f) { return NULL; }
void jack_free(void* ptr) {}

// Missing IO/Port Stubs
uint32_t jack_frame_time(jack_client_t c) { return 0; }
void jack_port_set_latency(jack_port_t p, jack_nframes_t n) {}
jack_port_t jack_port_register(jack_client_t c, const char* n, const char* t, unsigned long f, unsigned long b) { return (void*)1; }

// Client/Callback Stubs
jack_client_t jack_client_new(const char* name) { return (void*)1; }
int jack_set_xrun_callback(jack_client_t c, int (*cb)(void*), void* arg) { return 0; }
int jack_set_buffer_size_callback(jack_client_t c, int (*cb)(jack_nframes_t, void*), void* arg) { return 0; }

// Threading Stubs
pthread_t jack_client_thread_id(jack_client_t c) { 
    return pthread_self(); 
}


int jack_is_realtime(jack_client_t c) { return 0; }

// Haiku/Pthread shim
int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy) { return 0; }


// STUB the scheduling functions entirely
// By defining these in your stub library, they will override the 
// system calls and prevent JAMin from actually touching the scheduler.
int pthread_getschedparam(pthread_t thread, int *policy, struct sched_param *param) {
    if (policy) *policy = 0;
    if (param) param->sched_priority = 0;
    return 0;
}

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param) {
    return 0;
}

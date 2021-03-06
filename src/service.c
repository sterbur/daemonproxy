/* service.c - routines for service objects
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#include "config.h"
#include "daemonproxy.h"
#include "Contained_RBTree.h"

// Describes a service, complete with metadata,
// argument list, file descriptor specification,
// and a state machine for watching the PID.

#define SVC_STATE_UNDEF         0
#define SVC_STATE_DOWN          1
#define SVC_STATE_START         2
#define SVC_STATE_UP            3
#define SVC_STATE_REAPED        4

struct service_s {
	int state;
	char name_buf[NAME_BUF_SIZE];
	strseg_t
		name,              // constant.  points to name_buf
		vars;              // dynamic, unless service pool feature used.
	RBTreeNode             // nodes for Red/Black tree indexing
		name_index_node, 
		pid_index_node;
	struct service_s       // doubly linked lists
		**active_prev_ptr, *active_next,
		**sigwake_prev_ptr, *sigwake_next;
	pid_t pid;
	bool auto_restart: 1,
		sigwake: 1,
		uses_control_event: 1,
		uses_control_cmd: 1,
		uses_control_socket: 1;
	int wait_status;
	int64_t  start_time;   // 32-bit-precision fixed point fraction
	int64_t  reap_time;
	int64_t  restart_interval;
	sigset_t autostart_signals;
};

// Service list - a vector of service references.
service_t
	**svc_list= NULL;
int
	svc_list_count= 0,
	svc_list_limit= 0;

// Service pool is an optional feature where all services are allocated from
// a single chunk of memory.  Nothing is resizable when using this feature.
void *svc_pool= NULL;
int svc_pool_size_each= 0;

RBTree svc_by_name_index;           // sorted index by name
RBTree svc_by_pid_index;            // sorted index by PID (only if running)
service_t *svc_active_list= NULL;   // linked list of services that need processed each iteration
service_t *svc_sigwake_list= NULL;  // linked list of services that can wake via signals
int64_t svc_last_signal_ts= 0;      // last signal we saw, for triggering services.

static service_t *svc_new(strseg_t name);
static void svc_ctor(service_t *svc, strseg_t name);
static void svc_dtor(service_t *svc);

static bool svc_list_resize(int new_limit);
static void svc_notify_state(service_t *svc);
static void svc_change_pid(service_t *svc, pid_t pid);
static bool svc_do_fork(service_t *svc);
static void svc_do_exec(service_t *svc);
static void svc_set_active(service_t *svc, bool activate);
static void svc_set_sigwake(service_t *svc, bool sigwake);
static bool svc_check_sigwake(service_t *svc);

int svc_by_name_compare(void *data, RBTreeNode *node) {
	strseg_t *name= (strseg_t*) data;
	service_t *obj= (service_t*) node->Object;
	return strseg_cmp(*name, obj->name);
}

int svc_by_pid_compare(void *key, RBTreeNode *node) {
	pid_t a= * (pid_t*) key;
	pid_t b= ((service_t*) node->Object)->pid;
	return a < b? -1 : a > b? 1 : 0;
}

void svc_init() {
	RBTree_Init( &svc_by_name_index, svc_by_name_compare );
	RBTree_Init( &svc_by_pid_index,  svc_by_pid_compare );
}

bool svc_preallocate(int count, int data_size_each) {
	int i, size_each;
	assert(svc_list == NULL);
	assert(svc_pool == NULL);
	assert(data_size_each > 0);

	size_each= sizeof(service_t) + data_size_each;
	size_each= ((size_each - 1) | 0xF) + 1; // round to 16
	
	if (!svc_list_resize(count))
		return false;
	
	if (!(svc_pool= malloc(count * size_each)))
		return false;
	svc_pool_size_each= size_each;
	for (i= 0; i < count; i++)
		svc_list[i]= (service_t*) (((char*) svc_pool) + size_each * i);
	return true;
}

bool svc_list_resize(int new_limit) {
	service_t **new_list;

	new_list= realloc(svc_list, new_limit * sizeof(fd_t*));
	if (!new_list)
		return false;
	svc_list= new_list;
	svc_list_limit= new_limit;
	return true;
}

service_t *svc_new(strseg_t name) {
	service_t *svc;
	
	assert(name.len < NAME_BUF_SIZE);
	
	// enlarge the service vector if needed (and not using a pool)
	if (svc_list_count >= svc_list_limit)
		if (svc_pool || !svc_list_resize(svc_list_limit + 32))
			return NULL;

	// allocate space (unless using a pool)
	if (svc_pool)
		svc= svc_list[svc_list_count++];
	else {
		if (!(svc= (service_t*) malloc(sizeof(service_t))))
			return NULL;
		svc_list[svc_list_count++]= svc;
	}
	
	svc_ctor(svc, name);
	return svc;
}

void svc_delete(service_t *svc) {
	int i;
	
	svc_dtor(svc);
	// remove the pointer from fd_list and free the mem (or swap within list, for obj pool)
	for (i= 0; i < svc_list_count; i++) {
		if (svc_list[i] == svc) {
			svc_list[i]= svc_list[--svc_list_count];
			// free the memory (unless object pool)
			if (svc_pool)
				svc_list[svc_list_count]= svc;
			else {
				svc_list[svc_list_count]= NULL;
				free(svc);
			}
			break;
		}
	}
}

// Requires a buffer as large as sizeof(service_t) + name.len + 1 !
void svc_ctor(service_t *svc, strseg_t name) {
	assert(name.len < NAME_BUF_SIZE);

	memset(svc, 0, sizeof(service_t));
	svc->state= SVC_STATE_DOWN;
	
	sigemptyset(&svc->autostart_signals); // probably redundant, but obeying API...
	
	memcpy(svc->name_buf, name.data, name.len);
	svc->name= (strseg_t){ svc->name_buf, name.len };
	
	if (svc_pool) {
		// When part of a pool, the vars are allocated immediately after the name
		svc->vars.data= svc->name.data + svc->name.len + 1;
	}
	
	RBTreeNode_Init( &svc->name_index_node );
	svc->name_index_node.Object= svc;

	RBTreeNode_Init( &svc->pid_index_node );
	svc->pid_index_node.Object= svc;
	
	RBTree_Add( &svc_by_name_index, &svc->name_index_node, &name );
	// unless NDEBUG:
		svc_check(svc);
}

void svc_dtor(service_t *svc) {
	svc_set_active(svc, false); // remove from 'active' linked list
	svc_set_sigwake(svc, false); // remove from 'sigwake' linked list
	if (svc->pid)
		RBTreeNode_Prune( &svc->pid_index_node );
	RBTreeNode_Prune( &svc->name_index_node );
	// Free the variables pool, but only if service pool feature not enabled
	if (!svc_pool && svc->vars.data)
		free((char*)svc->vars.data);
}

const char * svc_get_name(service_t *svc) {
	return svc->name.data;
}

bool svc_check_name(strseg_t name) {
	const char *p, *lim;
	if (name.len >= NAME_BUF_SIZE)
		return false;
	for (p= name.data, lim= p+name.len; p < lim; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
			return false;
	return true;
}

pid_t   svc_get_pid(service_t *svc) {
	return svc->pid;
}
int     svc_get_wstat(service_t *svc) {
	return svc->wait_status;
}
int64_t svc_get_up_ts(service_t *svc) {
	return svc->start_time;
}
int64_t svc_get_reap_ts(service_t *svc) {
	return svc->reap_time;
}

/** Get a named variable.
 *
 * Returns true if found or false if not.  If true, and value_out is given,
 * value_out is pointed to the string which is also NUL terminated.
 */
static bool svc_get_var(service_t *svc, strseg_t name, strseg_t *value_out) {
	strseg_t val, key, vars= svc->vars;

	assert(name.len >= 0);

	while (vars.len > 0 && strseg_tok_next(&vars, '\0', &val)) {
		if (strseg_tok_next(&val, '=', &key) && 0 == strseg_cmp(key, name)) {
			if (value_out) *value_out= val;
			return true;
		}
	}
	return false;
}

/** Set the named variable to a new value.
 *
 * The variables are packed back to back in a buffer of name=value strings.
 * It could be expensive to modify these variables, but the service variable
 * pool is small (200 bytes or so by default) and changes are infrequent,
 * so this should be sufficient.
 */
static bool svc_set_var(service_t *svc, strseg_t name, strseg_t *value) {
	int sizediff, buf_size;
	char *buf= (char*) svc->vars.data;
	bool found= false;
	strseg_t oldval, key, tail= svc->vars;

	assert(name.len >= 0);
	assert(!value || value->len >= 0);

	// See if we have a variable of this name yet
	while (tail.len > 0 && strseg_tok_next(&tail, '\0', &oldval)) {
		if (strseg_tok_next(&oldval, '=', &key) && 0 == strseg_cmp(key, name)) {
			found= true;
			break;
		}
	}
	sizediff= found? ( value? value->len - oldval.len : -(oldval.len + 1 + name.len + 1) )
		: ( value? name.len + 1 + value->len + 1 : 0 );

	// make sure we have room for new value
	if (sizediff > 0) {
		// Objects in pool cannot be resized
		if (svc_pool) {
			buf_size= svc_pool_size_each - (buf - (char*)svc);
			if (svc->vars.len + sizediff > buf_size)
				return false;
		}
		// else just realloc
		else {
			buf= (char*) realloc(buf, svc->vars.len + sizediff);
			if (!buf)
				return false;
			// Translate our pointers into the new buffer
			tail.data += (buf - svc->vars.data);
			oldval.data += (buf - svc->vars.data);
			svc->vars.data= buf;
		}
	}
	// adjust recorded size of variables pool, since we can't fail below this point
	svc->vars.len += sizediff;

	// if tail of buffer needs to move, move it
	if (sizediff && tail.len > 0)
		memmove((char*)tail.data + sizediff, tail.data, tail.len);

	// if we're adding a new var, set up the "name=" portion at the end of the buffer
	if (!found && value) {
		memcpy((char*)tail.data, name.data, name.len);
		((char*)tail.data)[name.len]= '=';
		oldval.data= tail.data + name.len + 1;
	}

	// If we have a value, overwrite the old one
	if (value) {
		memcpy((char*)oldval.data, value->data, value->len);
		((char*)oldval.data)[value->len]= '\0';
	}

	// unless NDEBUG:
		svc_check(svc);
	return true;
}

const char * svc_get_tags(service_t *svc) {
	strseg_t val;
	return svc_get_var(svc, STRSEG("tags"), &val)? val.data : "";
}

/** Set the string for the service's tags
 * This string is concatenated with args and fds in a single buffer.
 * This can be slightly expensive, but args and fds are typically static.
 */
bool svc_set_tags(service_t *svc, strseg_t new_tags) {
	return svc_set_var(svc, STRSEG("tags"), new_tags.len <= 0? NULL : &new_tags);
}

const char * svc_get_argv(service_t *svc) {
	strseg_t val;
	return svc_get_var(svc, STRSEG("args"), &val)? val.data : "";
}

/** Set the string for the service's argument list
 * This string is concatenated with tags and fds in a single buffer.
 * This can be slightly expensive, but args and fds are typically static.
 */
bool svc_set_argv(service_t *svc, strseg_t new_argv) {
	return svc_set_var(svc, STRSEG("args"), new_argv.len <= 0? NULL : &new_argv);
}

const char * svc_get_fds(service_t *svc) {
	strseg_t val;
	return svc_get_var(svc, STRSEG("fds"), &val)? val.data : "null\tnull\tnull";
}

/** Set the string for the service's file descriptor specification
 * This string is concatenated with meta and argv in a single buffer.
 * This can be slightly expensive, but args and fds are typically static.
 */
bool svc_set_fds(service_t *svc, strseg_t new_fds) {
	strseg_t name;
	
	if (new_fds.len < 0) new_fds.len= 0;
	// The default value is "null null null", but we don't want to waste bytes on it
	// or have to initialize it.  So "null null null" is represented by being unset.
	if (strseg_cmp(new_fds, STRSEG("null\tnull\tnull")) == 0)
		svc_set_var(svc, STRSEG("fds"), NULL);
	else if (!svc_set_var(svc, STRSEG("fds"), &new_fds))
		return false;
	
	// fds have been changed, so re-evaluate whether they are using
	// the special control handles.
	svc->uses_control_event= false;
	svc->uses_control_cmd= false;
	svc->uses_control_socket= false;
	while (strseg_tok_next(&new_fds, '\t', &name)) {
		if (strseg_cmp(name, STRSEG("control.event")) == 0)
			svc->uses_control_event= true;
		if (strseg_cmp(name, STRSEG("control.cmd")) == 0)
			svc->uses_control_cmd= true;
		if (strseg_cmp(name, STRSEG("control.socket")) == 0)
			svc->uses_control_socket= true;
	}
	return true;
}

int64_t svc_get_restart_interval(service_t *svc) {
	return svc->restart_interval;
}

bool svc_set_restart_interval(service_t *svc, int64_t interval) {
	if ((interval >> 32) < 1)
		return false;
	svc->restart_interval= interval;
	return true;
}

const char * svc_get_triggers(service_t *svc) {
	strseg_t val;
	return svc_get_var(svc, STRSEG("triggers"), &val)? val.data : "";
}

bool svc_set_triggers(service_t *svc, strseg_t triggers_tsv) {
	strseg_t list= triggers_tsv, trigger;
	sigset_t sigs;
	int signum;
	bool autostart= false, enable_sigs= false;
	
	// convert triggers to bit flags
	sigemptyset(&sigs);
	while (strseg_tok_next(&list, '\t', &trigger) && trigger.len > 0) {
		if (0 == strseg_cmp(trigger, STRSEG("always")))
			autostart= true;
		else if ((signum= sig_num_by_name(trigger)) > 0) {
			if (sigaddset(&sigs, signum) < 0)
				return false;
			enable_sigs= true;
		}
		else
			return false;
	}

	if (!svc_set_var(svc, STRSEG("triggers"), triggers_tsv.len <= 0? NULL : &triggers_tsv))
		return false;

	svc->auto_restart= autostart;
	svc->autostart_signals= sigs;
	svc_set_sigwake(svc, enable_sigs);
	
	// finally, if a relevant signal is un-cleared, start the service.
	if (svc->auto_restart || svc_check_sigwake(svc)) {
		log_trace("Service needs started now");
		svc_handle_start(svc, wake->now);
	}

	return true;
}
	
static void svc_set_sigwake(service_t *svc, bool sigwake) {
	svc->sigwake= sigwake;
	// Add or remove this service from the sigwake list, as needed.
	if (sigwake && !svc->sigwake_prev_ptr) {
		log_trace("Adding service to sigwake_list");
		svc->sigwake_next= svc_sigwake_list;
		if (svc_sigwake_list)
			svc_sigwake_list->sigwake_prev_ptr= &svc->sigwake_next;
		svc_sigwake_list= svc;
		svc->sigwake_prev_ptr= &svc_sigwake_list;
	}
	else if (!sigwake && svc->sigwake_prev_ptr) {
		log_trace("Removing service from sigwake_list");
		if (svc->sigwake_next)
			svc->sigwake_next->sigwake_prev_ptr= svc->sigwake_prev_ptr;
		*svc->sigwake_prev_ptr= svc->sigwake_next;
		svc->sigwake_prev_ptr= NULL;
	}
}

static bool svc_check_sigwake(service_t *svc) {
	int signum, sig_count;
	int64_t sig_ts;

	if (!svc->sigwake) return false;

	sig_ts= 0;
	while (sig_get_new_events(sig_ts, &signum, &sig_ts, &sig_count))
		if (sigismember(&svc->autostart_signals, signum))
			return true;
	return false;
}

bool svc_handle_start(service_t *svc, int64_t when) {
	if (svc->state != SVC_STATE_DOWN && svc->state != SVC_STATE_START) {
		log_debug("Can't start service \"%s\": state is %d", svc_get_name(svc), svc->state);
		return false;
	}
	
	if (when - wake->now > 0) {
		log_debug("start service \"%s\" in %d seconds", svc_get_name(svc), (int)((when - wake->now) >> 32));
	}
	else {
		log_debug("start service \"%s\" now", svc_get_name(svc));
		when= wake->now;
	}
	svc->state= SVC_STATE_START;
	svc->start_time= (when == 0? 1 : when); // 0 means undefined
	svc_change_pid(svc, 0);
	svc->reap_time= 0;
	svc->wait_status= -1;
	svc_set_active(svc, true);
	svc_notify_state(svc);
	wake->next= wake->now;
	return true;
}

bool svc_cancel_start(service_t *svc) {
	if (svc->state != SVC_STATE_START) {
		log_debug("Can't cancel start for service \"%s\": state is %d", svc_get_name(svc), svc->state);
		return false;
	}
	
	svc->state= SVC_STATE_DOWN;
	svc->start_time= 0;
	svc_set_active(svc, false);
	svc_notify_state(svc);
	return true;
}

/** Handle the case where a service's pid was reaped with wait().
 * This wakes up the service state machine, to possibly restart the daemon.
 * It is assumed that this is called by main() before iterating the active services.
 */
void svc_handle_reaped(service_t *svc, int wstat) {
	if (svc->state == SVC_STATE_UP) {
		log_trace("Setting service \"%s\" state to reaped", svc_get_name(svc));
		svc->wait_status= wstat;
		svc->state= SVC_STATE_REAPED;
		svc->reap_time= wake->now;
		svc_set_active(svc, true);
		wake->next= wake->now;
	}
	else log_trace("Service \"%s\" pid %d reaped, but service is not up", svc_get_name(svc), svc->pid);
}

/** Send a signal to a service iff it is running.
 */
bool svc_send_signal(service_t *svc, int signum, bool group) {
	if (!svc || svc->pid <= 0) return false;
	
	log_debug("Sending signal %d to service \"%s\" pid %d", signum, svc_get_name(svc), (int)svc->pid);
	return 0 == (group? killpg(svc->pid, signum) : kill(svc->pid, signum));
}

/** Activate or deactivate a service.
 * This simply inserts or removes the service from a linked list.
 * Each service in the "active" list get processed each time the main loop wakes up.
 */
void svc_set_active(service_t *svc, bool activate) {
	if (activate && !svc->active_prev_ptr) {
		// Insert node at head of doubly-linked list
		svc->active_prev_ptr= &svc_active_list;
		svc->active_next= svc_active_list;
		if (svc_active_list)
			svc_active_list->active_prev_ptr= &svc->active_next;
		svc_active_list= svc;
	}
	else if (!activate && svc->active_prev_ptr) {
		// remove node from doubly linked list
		if (svc->active_next)
			svc->active_next->active_prev_ptr= svc->active_prev_ptr;
		*svc->active_prev_ptr= svc->active_next;
		svc->active_prev_ptr= NULL;
	}
	// unless NDEBUG:
		svc_check(svc);
}

/** Run the state machine for each active service.
 * Services might set themselves back to inactive during this loop.
 */
void svc_run_active() {
	service_t *svc, *next;
	int signum, sig_count;
	int64_t sig_ts;

	// For any new signal received, check if it wakes any services
	if (svc_sigwake_list)
		while (sig_get_new_events(svc_last_signal_ts, &signum, &sig_ts, &sig_count)) {
			svc= svc_sigwake_list;
			while (svc) {
				next= svc->sigwake_next;
				if (sigismember(&svc->autostart_signals, signum))
					svc_handle_start(svc, wake->now);
				svc= next;
			}
			svc_last_signal_ts= sig_ts;
		}

	// run state machine for any active service
	svc= svc_active_list;
	while (svc) {
		next= svc->active_next;
		svc_run(svc);
		svc= next;
	}
}

/** Run the state machine for one service.
 */
void svc_run(service_t *svc) {
	re_switch_state:
	log_trace("service %s state = %d", svc_get_name(svc), svc->state);
	switch (svc->state) {
	case SVC_STATE_START:
		// if not wake time yet,
		if (svc->start_time - wake->now > 0) {
			// set main-loop wake time if we're next
			if (svc->start_time - wake->next < 0)
				wake->next= svc->start_time;
			// ensure listed as active
			svc_set_active(svc, true);
			break;
		}
		
		// else we've reached the time to retry
		if (!svc_do_fork(svc)) {
			log_info("will retry in %d seconds", (int)( FORK_RETRY_DELAY >> 32 ));
			svc_handle_start(svc, wake->now + FORK_RETRY_DELAY);
			goto re_switch_state;
		}
		
		// service is started
		svc->start_time= (wake->now? wake->now : 1); // time != 0 hack
		svc->state= SVC_STATE_UP;
		svc_notify_state(svc);
	case SVC_STATE_UP:
		svc_set_active(svc, false);
		// waitpid in main loop will re-activate us and set state to REAPED
		break;
	case SVC_STATE_REAPED:
		svc_notify_state(svc);
		svc->state= SVC_STATE_DOWN;
		if (svc->auto_restart || svc_check_sigwake(svc)) {
			// if restarting too fast, delay til future
			svc_handle_start(svc, 
				(svc->reap_time - svc->start_time < svc->restart_interval)?
				wake->now + svc->restart_interval : wake->now);
			svc_notify_state(svc);
		}
		goto re_switch_state;
	case SVC_STATE_DOWN:
		svc_set_active(svc, false);
		break;
	// We can only arrive here as a result of a bug.  Catch it with asserts.
	case SVC_STATE_UNDEF:
		assert(svc->state != SVC_STATE_UNDEF);
	default:
		assert(0);
	}
	// unless NDEBUG:
		svc_check(svc);
}

bool svc_do_fork(service_t *svc) {
	pid_t pid;
	int sockets[2]= { -1, -1 };
	controller_t *ctl= NULL;
	bool want_ctl_read= svc->uses_control_socket || svc->uses_control_event;
	bool want_ctl_write= svc->uses_control_socket || svc->uses_control_cmd;
	
	// If this service uses the control.{socket,cmd,event} file handles,
	// then we need to create a socket, and attach to a new controller
	if (svc->uses_control_socket || svc->uses_control_event || svc->uses_control_cmd) {
		// We need a controller object, of which there are a fixed number
		// Do we have one?  And can we create the sockets?
		if (!(ctl= ctl_alloc())) {
			ctl_free(ctl);
			ctl= NULL;
			log_error("can't allocate controller object");
			goto fail;
		}
		if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) {
			log_error("can't create socketpair: %s", strerror(errno));
			goto fail;
		}
		if (!ctl_ctor(ctl, want_ctl_write? sockets[0] : -1, want_ctl_read? sockets[0] : -1)) {
			log_error("can't initialize controller");
			goto fail;
		}
		
		// If the service is only using one of control.event or control.cmd, then we
		// shut down the unused direction so that it doesn't accidentally fill up
		// with buffered data that will never be read.  i.e. simulate a single pipe.
		if (!svc->uses_control_socket) {
			// 0 is ours, 1 is theirs.
			if (!want_ctl_read) {
				shutdown(sockets[1], SHUT_RD);
				shutdown(sockets[0], SHUT_WR);
			}
			if (!want_ctl_write) {
				shutdown(sockets[1], SHUT_WR);
				shutdown(sockets[0], SHUT_RD);
			}
		}
	}
	
	if ((pid= fork()) < 0) {
		log_error("fork failed: %s", strerror(errno));
		goto fail;
	}
	
	// Are we the client?  perform exec
	if (pid == 0) {
		if (sockets[0] >= 0)
			close(sockets[0]);
		if (sockets[1] >= 0) {
			// Store the FD number of the client's socket in each of the FD objects
			// which svc_do_exec might be looking at.
			fd_set_fdnum(fd_by_name(STRSEG("control.socket")), sockets[1]);
			fd_set_fdnum(fd_by_name(STRSEG("control.cmd")), sockets[1]);
			fd_set_fdnum(fd_by_name(STRSEG("control.event")), sockets[1]);
		}
		svc_do_exec(svc);
		// never returns
		assert(0);
	}
	
	if (sockets[1] >= 0)
		close(sockets[1]);

	svc_change_pid(svc, pid);
	
	return true;

	fail: // cleanup based on what was initialized
	
	if (ctl) {
		ctl_dtor(ctl); // this closes sockets[0]
		ctl_free(ctl);
	}
	else if (sockets[0] >= 0)
		close(sockets[0]);
	if (sockets[1] >= 0)
		close(sockets[1]);
	return false;
}

/** Perform the exec() to launch the service's daemon (or runscript)
 * This sets up FDs, and calls exec() with the argv for the service.
 */
void svc_do_exec(service_t *svc) {
	int fd_count, arg_count, i;
	int *fd_list= NULL;
	fd_t *fd;
	char **argv, *arg_spec, *p;
	strseg_t fd_spec, tmp, fd_name;

	// clear signal mask and handlers
	log_trace("resetting signal mask");
	sig_reset_for_exec();
	
	fd_spec.data= svc_get_fds(svc);
	fd_spec.len= strlen(fd_spec.data);
	fd_count= 0;
	if (fd_spec.len) {
		// count our file descriptors, to allocate buffer
		tmp= fd_spec;
		while (strseg_tok_next(&tmp, '\t', &fd_name))
			fd_count++;
		fd_list= alloca(fd_count);
		// now iterate again to resolve them from name to number
		fd_count= 0;
		tmp= fd_spec;
		while (strseg_tok_next(&tmp, '\t', &fd_name)) {
			if (fd_name.len <= 0)
				log_warn("ignoring zero-length file descriptor name");
			else if (fd_name.len == 1 && fd_name.data[0] == '-')
				fd_list[fd_count++]= -1; // dash means "closed"
			else if ((fd= fd_by_name(fd_name)))
				fd_list[fd_count++]= fd_get_fdnum(fd);
			else {
				log_error("file descriptor \"%.*s\" does not exist", fd_name.len, fd_name.data);
				abort();
			}
		}
	}
	
	// Now move them into correct places
	// But first, we need to make sure all the file descriptors we're about to copy
	//   are out of the way...
	for (i= 0; i < fd_count; i++) {
		// If file descriptor is less than the max destination, dup it to a higher number
		// We have no way to know which higher numbers are safe, so repeat until we get one.
		log_trace("fd_list[%d]= %d", i, fd_list[i]);
		while (fd_list[i] >= 0 && fd_list[i] < fd_count) {
			fd_list[i]= dup(fd_list[i]);
			if (fd_list[i] < 0) {
				log_error("Failed to dup file descriptor %d", fd_list[i]);
				abort();
			}
			log_trace("  dup'd to %d", fd_list[i]);
		}
	}
	// Now dup2 each into its correct slot, and close the rest
	for (i= 0; i < fd_count; i++) {
		if (fd_list[i] >= 0) {
			if (dup2(fd_list[i], i) < 0) {
				log_error("Failed to dup file descriptor %d to %d", fd_list[i], i);
				abort();
			}
		}
		else close(i);
	}
	// close all fd we aren't keeping
	while (i < FD_SETSIZE) close(i++);
	
	// just modify the buffer in the service object, since we're execing soon
	arg_spec= (char*) svc_get_argv(svc);
	// convert argv into pointers
	// count, allocate, then populate
	for (arg_count= 1, p= arg_spec; *p; p++)
		if (*p == '\t')
			arg_count++;
	argv= alloca(arg_count+1);
	// then populate
	i= 0;
	for (argv[0]= p= arg_spec; *p; p++)
		if (*p == '\t') {
			*p= '\0';
			argv[++i]= p+1;
		}
	argv[++i]= NULL;
	
	execvp(argv[0], argv);
	log_error("exec(%s, ...) failed: %s", argv[0], strerror(errno));
	_exit(EXIT_INVALID_ENVIRONMENT);
}
	
void svc_notify_state(service_t *svc) {
	log_trace("service %s state = %d", svc_get_name(svc), svc->state);
	ctl_notify_svc_state(NULL, svc->name.data, svc->start_time, svc->reap_time, svc->wait_status, svc->pid);
}

service_t *svc_by_name(strseg_t name, bool create) {
	RBTreeSearch s= RBTree_Find( &svc_by_name_index, &name );
	if (s.Relation == 0)
		return (service_t*) s.Nearest->Object;
	// if create requested, create a new service by this name
	// (if name is valid)
	if (create && svc_check_name(name))
		return svc_new(name);

	return NULL;
}

void svc_change_pid(service_t *svc, pid_t pid) {
	if (svc->pid)
		RBTreeNode_Prune( &svc->pid_index_node );
	svc->pid= pid;
	if (svc->pid) {
		RBTreeNode_Init( &svc->pid_index_node );
		svc->pid_index_node.Object= svc;
		RBTree_Add( &svc_by_pid_index, &svc->pid_index_node, &svc->pid );
	}
	// unless NDEBUG:
		svc_check(svc);
}

service_t *svc_by_pid(pid_t pid) {
	RBTreeSearch s= RBTree_Find( &svc_by_pid_index, &pid );
	if (s.Relation == 0)
		return (service_t*) s.Nearest->Object;
	return NULL;
}

service_t * svc_iter_next(service_t *svc, const char *from_name) {
	RBTreeNode *node;
	strseg_t n;
	log_trace("next service from %p or \"%s\"", svc, from_name);
	if (svc) {
		node= RBTreeNode_GetNext(&svc->name_index_node);
	} else {
		n= STRSEG(from_name);
		RBTreeSearch s= RBTree_Find( &svc_by_name_index, &n );
		log_trace("find(\"%.*s\"): { %d, %p }", n.len, n.data, s.Relation, s.Nearest);
		if (s.Nearest == NULL)
			node= NULL;
		else if (s.Relation < 0) // If key is less than returned node,
			node= s.Nearest;     // then we've got the "next node"
		else                    // else we got the "prev node" and need the next one
			node= RBTreeNode_GetNext(s.Nearest);
	}
	return node? (service_t *) node->Object : NULL;
}

#ifndef NDEBUG
void svc_check(service_t *svc) {
	assert(svc != NULL);

	assert(svc->name.len > 0);
	assert(svc->name.len < NAME_BUF_SIZE);
	assert(svc->name.data == svc->name_buf);
	assert(svc->name.data[svc->name.len] == 0);

	assert(svc->vars.len >= 0);
	if (svc->vars.len) {
		assert(svc->vars.data);
		assert(svc->vars.data[svc->vars.len-1] == 0);
	}
	if (svc_pool) {
		assert(svc->name.data + svc->name.len + 1 == svc->vars.data);
		assert( ((char*)svc) + svc_pool_size_each >= svc->vars.data + svc->vars.len );
	}

	assert(svc->name_index_node.Color == RBTreeNode_Black || svc->name_index_node.Color == RBTreeNode_Red);
	if (svc->pid)
		assert(svc->pid_index_node.Color == RBTreeNode_Black || svc->pid_index_node.Color == RBTreeNode_Red);
	else
		assert(svc->pid_index_node.Color == RBTreeNode_Unassigned);
}
#endif

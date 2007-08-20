#include <switch.h>
#include <switch_cpp.h>

#ifdef _MSC_VER
#pragma warning(disable:4127 4003)
#endif

#define sanity_check(x) do { if (!session) { switch_log_printf(SWITCH_CHANNEL_LOG,SWITCH_LOG_ERROR, "session is not initalized\n"); return x;}} while(0)
#define sanity_check_noreturn do { if (!session) { switch_log_printf(SWITCH_CHANNEL_LOG,SWITCH_LOG_ERROR, "session is not initalized\n"); return;}} while(0)
#define init_vars() do { session = NULL; channel = NULL; uuid = NULL; tts_name = NULL; voice_name = NULL; memset(&args, 0, sizeof(args)); ap = NULL; caller_profile.source = "mod_unknown";  caller_profile.dialplan = ""; caller_profile.context = ""; caller_profile.caller_id_name = ""; caller_profile.caller_id_number = ""; caller_profile.network_addr = ""; caller_profile.ani = ""; caller_profile.aniii = ""; caller_profile.rdnis = "";  caller_profile.username = ""; on_hangup = NULL; cb_state.function = NULL; } while(0)



CoreSession::CoreSession()
{
	init_vars();
}

CoreSession::CoreSession(char *nuuid)
{
	init_vars();
    uuid = strdup(nuuid);
	if (session = switch_core_session_locate(uuid)) {
		channel = switch_core_session_get_channel(session);
    }
}

CoreSession::CoreSession(switch_core_session_t *new_session)
{
	init_vars();
	session = new_session;
	channel = switch_core_session_get_channel(session);
	switch_core_session_read_lock(session);
}

CoreSession::~CoreSession()
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CoreSession::~CoreSession desctructor");
	switch_channel_t *channel = NULL;

	if (session) {
		channel = switch_core_session_get_channel(session);
		if (channel && switch_test_flag(this, S_HUP)) {
			switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		}
		switch_core_session_rwunlock(session);
	}

	switch_safe_free(uuid);	
	switch_safe_free(tts_name);
	switch_safe_free(voice_name);
}

int CoreSession::answer()
{
    switch_status_t status;

	sanity_check(-1);
    status = switch_channel_answer(channel);
    return status == SWITCH_STATUS_SUCCESS ? 1 : 0;
}

int CoreSession::preAnswer()
{
    switch_status_t status;
	sanity_check(-1);
    status = switch_channel_pre_answer(channel);
    return status == SWITCH_STATUS_SUCCESS ? 1 : 0;
}

void CoreSession::hangup(char *cause)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CoreSession::hangup\n");
	sanity_check_noreturn;
    switch_channel_hangup(channel, switch_channel_str2cause(cause));
}

void CoreSession::setVariable(char *var, char *val)
{
	sanity_check_noreturn;
    switch_channel_set_variable(channel, var, val);
}

char *CoreSession::getVariable(char *var)
{
	sanity_check(NULL);
    return switch_channel_get_variable(channel, var);
}

void CoreSession::execute(char *app, char *data)
{
	const switch_application_interface_t *application_interface;
	sanity_check_noreturn;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CoreSession::execute.  app: %s data:%s\n", app, data);
	if ((application_interface = switch_loadable_module_get_application_interface(app))) {
		begin_allow_threads();
		switch_core_session_exec(session, application_interface, data);
		end_allow_threads();
	}
}

void CoreSession::setDTMFCallback(void *cbfunc, char *funcargs) {

	cb_state.funcargs = funcargs;
	cb_state.function = cbfunc;

	args.buf = &cb_state; 
	args.buflen = sizeof(cb_state);  // not sure what this is used for, copy mod_spidermonkey

    switch_channel_set_private(channel, "CoreSession", this);
        
	// we cannot set the actual callback to a python function, because
	// the callback is a function pointer with a specific signature.
	// so, set it to the following c function which will act as a proxy,
	// finding the python callback in the args callback args structure
	args.input_callback = dtmf_callback;  
	ap = &args;


}

int CoreSession::speak(char *text)
{
    switch_status_t status;
    switch_codec_t *codec;

	sanity_check(-1);

	// create and store an empty filehandle in callback args 
	// to workaround a bug in the presumptuous process_callback_result()
    switch_file_handle_t fh = { 0 };
	store_file_handle(&fh);

	if (!tts_name) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No TTS engine specified");
		return SWITCH_STATUS_FALSE;
	}

	if (!voice_name) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No TTS voice specified");
		return SWITCH_STATUS_FALSE;
	}

    codec = switch_core_session_get_read_codec(session);
	begin_allow_threads();
	status = switch_ivr_speak_text(session, tts_name, voice_name, codec->implementation->samples_per_second, text, ap);
	end_allow_threads();
    return status == SWITCH_STATUS_SUCCESS ? 1 : 0;
}

void CoreSession::set_tts_parms(char *tts_name_p, char *voice_name_p)
{
	sanity_check_noreturn;
	switch_safe_free(tts_name);
	switch_safe_free(voice_name);
    tts_name = strdup(tts_name_p);
    voice_name = strdup(voice_name_p);
}



int CoreSession::collectDigits(int timeout) {
	sanity_check(-1);
    begin_allow_threads();
	switch_ivr_collect_digits_callback(session, ap, timeout);
    end_allow_threads();
    return SWITCH_STATUS_SUCCESS;
} 

int CoreSession::getDigits(char *dtmf_buf, 
						   int buflen, 
						   int maxdigits, 
						   char *terminators, 
						   char *terminator, 
						   int timeout)
{
    switch_status_t status;
	sanity_check(-1);
	begin_allow_threads();

    status = switch_ivr_collect_digits_count(session, 
											 dtmf_buf,
											 (uint32_t) buflen,
											 (uint32_t) maxdigits, 
											 terminators, 
											 terminator, 
											 (uint32_t) timeout);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "getDigits dtmf_buf: %s\n", dtmf_buf);
	end_allow_threads();
    return status == SWITCH_STATUS_SUCCESS ? 1 : 0;
}

int CoreSession::transfer(char *extension, char *dialplan, char *context)
{
    switch_status_t status;
	sanity_check(-1);
    begin_allow_threads();
    status = switch_ivr_session_transfer(session, extension, dialplan, context);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "transfer result: %d\n", status);
    end_allow_threads();
    return status == SWITCH_STATUS_SUCCESS ? 1 : 0;
}

int CoreSession::playAndGetDigits(int min_digits, 
								  int max_digits, 
								  int max_tries, 
								  int timeout, 
								  char *terminators, 
								  char *audio_files, 
								  char *bad_input_audio_files, 
								  char *dtmf_buf, 
								  char *digits_regex)
{
    switch_status_t status;
	sanity_check(-1);
	begin_allow_threads();
    status = switch_play_and_get_digits( session, 
										 (uint32_t) min_digits,
										 (uint32_t) max_digits,
										 (uint32_t) max_tries, 
										 (uint32_t) timeout, 
										 terminators, 
										 audio_files, 
										 bad_input_audio_files, 
										 dtmf_buf, 
										 128, 
										 digits_regex);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "playAndGetDigits dtmf_buf: %s\n", dtmf_buf);

	end_allow_threads();
    return status == SWITCH_STATUS_SUCCESS ? 1 : 0;
}

int CoreSession::streamFile(char *file, int starting_sample_count) {

    switch_status_t status;
    switch_file_handle_t fh = { 0 };
	char *prebuf;

    sanity_check(-1);
    fh.samples = starting_sample_count;
	store_file_handle(&fh);

    begin_allow_threads();
    status = switch_ivr_play_file(session, &fh, file, ap);
    end_allow_threads();

	if ((prebuf = switch_channel_get_variable(this->channel, "stream_prebuffer"))) {
        int maybe = atoi(prebuf);
        if (maybe > 0) {
            fh.prebuf = maybe;
        }
	}

    return status == SWITCH_STATUS_SUCCESS ? 1 : 0;

}

bool CoreSession::ready() {

	switch_channel_t *channel;

	if (!session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "You must call the session.originate method before calling this method!\n");
		return false;
	}

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);
	
	return switch_channel_ready(channel) != 0;


}

int CoreSession::originate(CoreSession *a_leg_session, 
						   char *dest, 
						   int timeout)
{

	switch_memory_pool_t *pool = NULL;
	switch_core_session_t *aleg_core_session = NULL;
	switch_call_cause_t cause;

	cause = SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;

	if (a_leg_session != NULL) {
		aleg_core_session = a_leg_session->session;
	}

	// this session has no valid switch_core_session_t at this point, and therefore
	// no valid channel.  since the threadstate is stored in the channel, and there 
	// is none, if we try to call begin_alllow_threads it will fail miserably.
	// use the 'a leg session' to do the thread swapping stuff.
    a_leg_session->begin_allow_threads();

	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "OH OH no pool\n");
		goto failed;
	}

	if (switch_ivr_originate(aleg_core_session, 
							 &session, 
							 &cause, 
							 dest, 
							 timeout,
							 NULL, 
							 NULL, 
							 NULL, 
							 &caller_profile) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Error Creating Outgoing Channel! [%s]\n", dest);
		goto failed;

	}

    a_leg_session->end_allow_threads();
	return SWITCH_STATUS_SUCCESS;

 failed:
    a_leg_session->end_allow_threads();
	return SWITCH_STATUS_FALSE;
}

int CoreSession::recordFile(char *file_name, int max_len, int silence_threshold, int silence_secs) 
{
	switch_file_handle_t fh = { 0 };
	switch_status_t status;

	fh.thresh = silence_threshold;
	fh.silence_hits = silence_secs;
	store_file_handle(&fh);
	begin_allow_threads();
	status = switch_ivr_record_file(session, &fh, file_name, &args, max_len);
	end_allow_threads();
    return status == SWITCH_STATUS_SUCCESS ? 1 : 0;

}

int CoreSession::flushEvents() 
{
	switch_event_t *event;
	switch_channel_t *channel;

	if (!session) {
		return SWITCH_STATUS_FALSE;
	}
	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	while (switch_core_session_dequeue_event(session, &event) == SWITCH_STATUS_SUCCESS) {
		switch_event_destroy(&event);
	}
	return SWITCH_STATUS_SUCCESS;
}

int CoreSession::flushDigits() 
{
	char buf[256];
	switch_size_t has;
	switch_channel_t *channel;


	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	while ((has = switch_channel_has_dtmf(channel))) {
		switch_channel_dequeue_dtmf(channel, buf, sizeof(buf));
	}
	return SWITCH_STATUS_SUCCESS;
}

int CoreSession::setAutoHangup(bool val) 
{
	if (!session) {
		return SWITCH_STATUS_FALSE;
	}	
	if (val) {
		switch_set_flag(this, S_HUP);
	} else {
		switch_clear_flag(this, S_HUP);
	}
	return SWITCH_STATUS_SUCCESS;
}

void CoreSession::setCallerData(char *var, char *val) {

	if (strcmp(var, "dialplan") == 0) {
		caller_profile.dialplan = val;
	}
	if (strcmp(var, "context") == 0) {
		caller_profile.context = val;
	}
	if (strcmp(var, "caller_id_name") == 0) {
		caller_profile.caller_id_name = val;
	}
	if (strcmp(var, "caller_id_number") == 0) {
		caller_profile.caller_id_number = val;
	}
	if (strcmp(var, "network_addr") == 0) {
		caller_profile.network_addr = val;
	}
	if (strcmp(var, "ani") == 0) {
		caller_profile.ani = val;
	}
	if (strcmp(var, "aniii") == 0) {
		caller_profile.aniii = val;
	}
	if (strcmp(var, "rdnis") == 0) {
		caller_profile.rdnis = val;
	}
	if (strcmp(var, "username") == 0) {
		caller_profile.username = val;
	}

}

void CoreSession::setHangupHook(void *hangup_func) {

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CoreSession::seHangupHook, hangup_func: %p\n", hangup_func);
    on_hangup = hangup_func;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    assert(channel != NULL);

    hook_state = switch_channel_get_state(channel);
    switch_channel_set_private(channel, "CoreSession", this);
    switch_core_event_hook_add_state_change(session, hanguphook);

}

/** \brief Store a file handle in the callback args
 * 
 * In a few of the methods like playFile and streamfile,
 * an empty switch_file_handle_t is created and passed
 * to core, and stored in callback args so that the callback
 * handler can retrieve it for pausing, ff, rewinding file ptr. 
 * 
 * \param fh - a switch_file_handle_t
 */
void CoreSession::store_file_handle(switch_file_handle_t *fh) {
    cb_state.extra = fh;  // set a file handle so callback handler can pause
    args.buf = &cb_state;     
    ap = &args;
}


/* ---- methods not bound to CoreSession instance ---- */


void console_log(char *level_str, char *msg)
{
    switch_log_level_t level = SWITCH_LOG_DEBUG;
    if (level_str) {
        level = switch_log_str2level(level_str);
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, level, msg);
	fflush(stdout); // TEMP ONLY!! SHOULD NOT BE CHECKED IN!!
}

void console_clean_log(char *msg)
{
    switch_log_printf(SWITCH_CHANNEL_LOG_CLEAN,SWITCH_LOG_DEBUG, msg);
}


char *api_execute(char *cmd, char *arg)
{
	switch_stream_handle_t stream = { 0 };
	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute(cmd, arg, NULL, &stream);
	return (char *) stream.data;
}

void api_reply_delete(char *reply)
{
	if (!switch_strlen_zero(reply)) {
		free(reply);
	}
}


void bridge(CoreSession &session_a, CoreSession &session_b)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "bridge called, session_a uuid: %s\n", session_a.get_uuid());
	switch_input_callback_function_t dtmf_func = NULL;
	switch_input_args_t args;

	session_a.begin_allow_threads();
	args = session_a.get_cb_args();  // get the cb_args data structure for session a
	dtmf_func = args.input_callback;   // get the call back function
	switch_ivr_multi_threaded_bridge(session_a.session, session_b.session, dtmf_func, args.buf, args.buf);
	session_a.end_allow_threads();

}


switch_status_t hanguphook(switch_core_session_t *session_hungup) 
{
	switch_channel_t *channel;
	CoreSession *coresession = NULL;
	switch_channel_state_t state;


	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "hangup_hook called\n");
	fflush(stdout);

	channel = switch_core_session_get_channel(session_hungup);
	assert(channel != NULL);

	state = switch_channel_get_state(channel);

	if ((coresession = (CoreSession *) switch_channel_get_private(channel, "CoreSession"))) {
		if (coresession->hook_state != state) {
			coresession->hook_state = state;
			coresession->check_hangup_hook();
		}
	}

	return SWITCH_STATUS_SUCCESS;
}


switch_status_t dtmf_callback(switch_core_session_t *session_cb, 
							  void *input, 
							  switch_input_type_t itype, 
							  void *buf,  
							  unsigned int buflen) {
	
	switch_channel_t *channel;
	CoreSession *coresession = NULL;
	switch_status_t result;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dtmf_callback called\n");
	fflush(stdout);

	channel = switch_core_session_get_channel(session_cb);
	assert(channel != NULL);

	coresession = (CoreSession *) switch_channel_get_private(channel, "CoreSession");
	if (!coresession) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid CoreSession\n");		
		return SWITCH_STATUS_FALSE;
	}

	result = coresession->run_dtmf_callback(input, itype);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "process_callback_result returned\n");
	if (result) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "process_callback_result returned: %d\n", result);
	}
	return result;

}



switch_status_t process_callback_result(char *ret, 
					struct input_callback_state *cb_state,
					switch_core_session_t *session) 
{
	
    switch_file_handle_t *fh = NULL;	   

    if (!cb_state) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Process callback result aborted because cb_state is null\n");
		return SWITCH_STATUS_FALSE;	
    }

    if (!cb_state->extra) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Process callback result aborted because cb_state->extra is null\n");
		return SWITCH_STATUS_FALSE;	
    }

    fh = (switch_file_handle_t *) cb_state->extra;    

    if (!fh) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Process callback result aborted because fh is null\n");
		return SWITCH_STATUS_FALSE;	
    }

    if (!fh->file_interface) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Process callback result aborted because fh->file_interface is null\n");
		return SWITCH_STATUS_FALSE;	
    }

    if (!ret) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Process callback result aborted because ret is null\n");
		return SWITCH_STATUS_FALSE;	
    }

    if (!session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Process callback result aborted because session is null\n");
		return SWITCH_STATUS_FALSE;	
    }


    if (!strncasecmp(ret, "speed", 4)) {
		char *p;

		if ((p = strchr(ret, ':'))) {
			p++;
			if (*p == '+' || *p == '-') {
				int step;
				if (!(step = atoi(p))) {
					step = 1;
				}
				fh->speed += step;
			} else {
				int speed = atoi(p);
				fh->speed = speed;
			}
			return SWITCH_STATUS_SUCCESS;
		}

		return SWITCH_STATUS_FALSE;

    } else if (!strcasecmp(ret, "pause")) {
		if (switch_test_flag(fh, SWITCH_FILE_PAUSE)) {
			switch_clear_flag(fh, SWITCH_FILE_PAUSE);
		} else {
			switch_set_flag(fh, SWITCH_FILE_PAUSE);
		}
		return SWITCH_STATUS_SUCCESS;
    } else if (!strcasecmp(ret, "stop")) {
		return SWITCH_STATUS_FALSE;
    } else if (!strcasecmp(ret, "restart")) {
		unsigned int pos = 0;
		fh->speed = 0;
		switch_core_file_seek(fh, &pos, 0, SEEK_SET);
		return SWITCH_STATUS_SUCCESS;
    } else if (!strncasecmp(ret, "seek", 4)) {
		switch_codec_t *codec;
		unsigned int samps = 0;
		unsigned int pos = 0;
		char *p;
		codec = switch_core_session_get_read_codec(session);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "got codec\n");
		if ((p = strchr(ret, ':'))) {
			p++;
			if (*p == '+' || *p == '-') {
				int step;
				if (!(step = atoi(p))) {
					step = 1000;
				}
				if (step > 0) {
					samps = step * (codec->implementation->samples_per_second / 1000);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "going to seek\n");
					switch_core_file_seek(fh, &pos, samps, SEEK_CUR);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "done seek\n");
				} else {
					samps = step * (codec->implementation->samples_per_second / 1000);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "going to seek\n");
					switch_core_file_seek(fh, &pos, fh->pos - samps, SEEK_SET);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "done seek\n");
				}
			} else {
				samps = atoi(p) * (codec->implementation->samples_per_second / 1000);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "going to seek\n");
				switch_core_file_seek(fh, &pos, samps, SEEK_SET);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "done seek\n");
			}
		}

		return SWITCH_STATUS_SUCCESS;
    }

    if (!strcmp(ret, "true") || !strcmp(ret, "undefined")) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "return success\n");
		return SWITCH_STATUS_SUCCESS;
    }

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "no match, return false\n");
    return SWITCH_STATUS_FALSE;


}


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 expandtab:
 */

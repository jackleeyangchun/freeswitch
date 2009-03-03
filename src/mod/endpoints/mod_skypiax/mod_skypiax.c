/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005/2006, Anthony Minessale II <anthmct@yahoo.com>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthmct@yahoo.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * This module (mod_skypiax) has been contributed by:
 *
 * Giovanni Maruzzelli (gmaruzz@gmail.com)
 *
 *
 * Further Contributors:
 *
 *
 *
 * mod_skypiax.c -- Skype compatible Endpoint Module
 *
 */

#include "skypiax.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_skypiax_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_skypiax_shutdown);
SWITCH_MODULE_DEFINITION(mod_skypiax, mod_skypiax_load, mod_skypiax_shutdown, NULL);

static struct {
  int debug;
  char *ip;
  int port;
  char *dialplan;
  char *destination;
  char *context;
  char *codec_string;
  char *codec_order[SWITCH_MAX_CODECS];
  int codec_order_last;
  char *codec_rates_string;
  char *codec_rates[SWITCH_MAX_CODECS];
  int codec_rates_last;
  unsigned int flags;
  int fd;
  int calls;
  char hold_music[256];
  private_t SKYPIAX_INTERFACES[SKYPIAX_MAX_INTERFACES];
  switch_mutex_t *mutex;
} globals;

switch_endpoint_interface_t *skypiax_endpoint_interface;
switch_memory_pool_t *skypiax_module_pool = NULL;
int running = 0;

SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_dialplan, globals.dialplan);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_context, globals.context);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_destination, globals.destination);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_codec_string, globals.codec_string);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_codec_rates_string,
                                  globals.codec_rates_string);

static switch_status_t channel_on_init(switch_core_session_t * session);
static switch_status_t channel_on_hangup(switch_core_session_t * session);
static switch_status_t channel_on_routing(switch_core_session_t * session);
static switch_status_t channel_on_exchange_media(switch_core_session_t * session);
static switch_status_t channel_on_soft_execute(switch_core_session_t * session);
static switch_call_cause_t channel_outgoing_channel(switch_core_session_t * session,
                                                    switch_event_t * var_event,
                                                    switch_caller_profile_t *
                                                    outbound_profile,
                                                    switch_core_session_t ** new_session,
                                                    switch_memory_pool_t ** pool,
                                                    switch_originate_flag_t flags);
static switch_status_t channel_read_frame(switch_core_session_t * session,
                                          switch_frame_t ** frame, switch_io_flag_t flags,
                                          int stream_id);
static switch_status_t channel_write_frame(switch_core_session_t * session,
                                           switch_frame_t * frame, switch_io_flag_t flags,
                                           int stream_id);
static switch_status_t channel_kill_channel(switch_core_session_t * session, int sig);

static switch_status_t skypiax_codec(private_t * tech_pvt, int sample_rate, int codec_ms)
{
  switch_core_session_t *session = NULL;

  if (switch_core_codec_init
      (&tech_pvt->read_codec, "L16", NULL, sample_rate, codec_ms, 1,
       SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
       NULL) != SWITCH_STATUS_SUCCESS) {
    ERRORA("Can't load codec?\n", SKYPIAX_P_LOG);
    return SWITCH_STATUS_FALSE;
  }

  if (switch_core_codec_init
      (&tech_pvt->write_codec, "L16", NULL, sample_rate, codec_ms, 1,
       SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
       NULL) != SWITCH_STATUS_SUCCESS) {
    ERRORA("Can't load codec?\n", SKYPIAX_P_LOG);
    switch_core_codec_destroy(&tech_pvt->read_codec);
    return SWITCH_STATUS_FALSE;
  }

  tech_pvt->read_frame.rate = sample_rate;
  tech_pvt->read_frame.codec = &tech_pvt->read_codec;

  session = switch_core_session_locate(tech_pvt->session_uuid_str);

  switch_core_session_set_read_codec(session, &tech_pvt->read_codec);
  switch_core_session_set_write_codec(session, &tech_pvt->write_codec);

  switch_core_session_rwunlock(session);

  return SWITCH_STATUS_SUCCESS;

}

void skypiax_tech_init(private_t * tech_pvt, switch_core_session_t * session)
{

  tech_pvt->read_frame.data = tech_pvt->databuf;
  tech_pvt->read_frame.buflen = sizeof(tech_pvt->databuf);
  switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED,
                    switch_core_session_get_pool(session));
  switch_mutex_init(&tech_pvt->flag_mutex, SWITCH_MUTEX_NESTED,
                    switch_core_session_get_pool(session));
  switch_core_session_set_private(session, tech_pvt);
  switch_copy_string(tech_pvt->session_uuid_str, switch_core_session_get_uuid(session),
                     sizeof(tech_pvt->session_uuid_str));
  if (skypiax_codec(tech_pvt, SAMPLERATE_SKYPIAX, 20) != SWITCH_STATUS_SUCCESS) {
    ERRORA("skypiax_codec FAILED\n", SKYPIAX_P_LOG);
  } else {
    DEBUGA_SKYPE("skypiax_codec SUCCESS\n", SKYPIAX_P_LOG);
  }

}

/* 
   State methods they get called when the state changes to the specific state 
   returning SWITCH_STATUS_SUCCESS tells the core to execute the standard state method next
   so if you fully implement the state you can return SWITCH_STATUS_FALSE to skip it.
*/
static switch_status_t channel_on_init(switch_core_session_t * session)
{
  switch_channel_t *channel;
  private_t *tech_pvt = NULL;

  tech_pvt = switch_core_session_get_private(session);
  assert(tech_pvt != NULL);

  channel = switch_core_session_get_channel(session);
  assert(channel != NULL);
  switch_set_flag_locked(tech_pvt, TFLAG_IO);

  /* Move channel's state machine to ROUTING. This means the call is trying
     to get from the initial start where the call because, to the point
     where a destination has been identified. If the channel is simply
     left in the initial state, nothing will happen. */
  switch_channel_set_state(channel, CS_ROUTING);
  switch_mutex_lock(globals.mutex);
  globals.calls++;
  switch_mutex_unlock(globals.mutex);

  DEBUGA_SKYPE("%s CHANNEL INIT\n", SKYPIAX_P_LOG, switch_channel_get_name(channel));
  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_hangup(switch_core_session_t * session)
{
  switch_channel_t *channel = NULL;
  private_t *tech_pvt = NULL;
  char msg_to_skype[256];

  channel = switch_core_session_get_channel(session);
  assert(channel != NULL);

  tech_pvt = switch_core_session_get_private(session);
  assert(tech_pvt != NULL);

  switch_clear_flag_locked(tech_pvt, TFLAG_IO);
  switch_clear_flag_locked(tech_pvt, TFLAG_VOICE);
  //switch_set_flag_locked(tech_pvt, TFLAG_HANGUP);

  if (strlen(tech_pvt->skype_call_id)) {
    //switch_thread_cond_signal(tech_pvt->cond);
    DEBUGA_SKYPE("hanging up skype call: %s\n", SKYPIAX_P_LOG, tech_pvt->skype_call_id);
    sprintf(msg_to_skype, "ALTER CALL %s HANGUP", tech_pvt->skype_call_id);
    skypiax_signaling_write(tech_pvt, msg_to_skype);
  }

  if (tech_pvt->read_codec.implementation) {
    switch_core_codec_destroy(&tech_pvt->read_codec);
  }

  if (tech_pvt->write_codec.implementation) {
    switch_core_codec_destroy(&tech_pvt->write_codec);
  }

  memset(tech_pvt->session_uuid_str, '\0', sizeof(tech_pvt->session_uuid_str));
  DEBUGA_SKYPE("%s CHANNEL HANGUP\n", SKYPIAX_P_LOG, switch_channel_get_name(channel));
  switch_mutex_lock(globals.mutex);
  globals.calls--;
  if (globals.calls < 0) {
    globals.calls = 0;
  }
  switch_mutex_unlock(globals.mutex);

  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_routing(switch_core_session_t * session)
{
  switch_channel_t *channel = NULL;
  private_t *tech_pvt = NULL;

  channel = switch_core_session_get_channel(session);
  assert(channel != NULL);

  tech_pvt = switch_core_session_get_private(session);
  assert(tech_pvt != NULL);

  DEBUGA_SKYPE("%s CHANNEL ROUTING\n", SKYPIAX_P_LOG, switch_channel_get_name(channel));

  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_execute(switch_core_session_t * session)
{

  switch_channel_t *channel = NULL;
  private_t *tech_pvt = NULL;

  channel = switch_core_session_get_channel(session);
  assert(channel != NULL);

  tech_pvt = switch_core_session_get_private(session);
  assert(tech_pvt != NULL);

  DEBUGA_SKYPE("%s CHANNEL EXECUTE\n", SKYPIAX_P_LOG, switch_channel_get_name(channel));

  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_kill_channel(switch_core_session_t * session, int sig)
{
  switch_channel_t *channel = NULL;
  private_t *tech_pvt = NULL;

  channel = switch_core_session_get_channel(session);
  assert(channel != NULL);

  tech_pvt = switch_core_session_get_private(session);
  assert(tech_pvt != NULL);

  switch (sig) {
  case SWITCH_SIG_KILL:
    DEBUGA_SKYPE("%s CHANNEL got SWITCH_SIG_KILL\n", SKYPIAX_P_LOG,
                 switch_channel_get_name(channel));
    switch_clear_flag_locked(tech_pvt, TFLAG_IO);
    switch_clear_flag_locked(tech_pvt, TFLAG_VOICE);
    switch_set_flag_locked(tech_pvt, TFLAG_HANGUP);
    break;
  case SWITCH_SIG_BREAK:
    DEBUGA_SKYPE("%s CHANNEL got SWITCH_SIG_BREAK\n", SKYPIAX_P_LOG,
                 switch_channel_get_name(channel));
    switch_set_flag_locked(tech_pvt, TFLAG_BREAK);
    break;
  default:
    break;
  }

  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_exchange_media(switch_core_session_t * session)
{
  private_t *tech_pvt = NULL;
  DEBUGA_SKYPE("CHANNEL LOOPBACK\n", SKYPIAX_P_LOG);
  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_soft_execute(switch_core_session_t * session)
{
  private_t *tech_pvt = NULL;
  DEBUGA_SKYPE("CHANNEL TRANSMIT\n", SKYPIAX_P_LOG);
  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_send_dtmf(switch_core_session_t * session,
                                         const switch_dtmf_t * dtmf)
{
  private_t *tech_pvt = switch_core_session_get_private(session);
  switch_assert(tech_pvt != NULL);

  DEBUGA_SKYPE("DTMF: %c\n", SKYPIAX_P_LOG, dtmf->digit);

  skypiax_senddigit(tech_pvt, dtmf->digit);

  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_read_frame(switch_core_session_t * session,
                                          switch_frame_t ** frame, switch_io_flag_t flags,
                                          int stream_id)
{
  switch_channel_t *channel = NULL;
  private_t *tech_pvt = NULL;
  switch_byte_t *data;

  channel = switch_core_session_get_channel(session);
  assert(channel != NULL);

  tech_pvt = switch_core_session_get_private(session);
  assert(tech_pvt != NULL);
  tech_pvt->read_frame.flags = SFF_NONE;
  *frame = NULL;

  if (!skypiax_audio_read(tech_pvt)) {

    ERRORA("skypiax_audio_read ERROR\n", SKYPIAX_P_LOG);

  } else {
    switch_set_flag_locked(tech_pvt, TFLAG_VOICE);
  }

  while (switch_test_flag(tech_pvt, TFLAG_IO)) {
    if (switch_test_flag(tech_pvt, TFLAG_BREAK)) {
      switch_clear_flag(tech_pvt, TFLAG_BREAK);
      DEBUGA_SKYPE("CHANNEL READ FRAME goto CNG\n", SKYPIAX_P_LOG);
      goto cng;
    }

    if (!switch_test_flag(tech_pvt, TFLAG_IO)) {
      DEBUGA_SKYPE("CHANNEL READ FRAME not IO\n", SKYPIAX_P_LOG);
      return SWITCH_STATUS_FALSE;
    }

    if (switch_test_flag(tech_pvt, TFLAG_IO) && switch_test_flag(tech_pvt, TFLAG_VOICE)) {
      switch_clear_flag_locked(tech_pvt, TFLAG_VOICE);
      if (!tech_pvt->read_frame.datalen) {
        DEBUGA_SKYPE("CHANNEL READ CONTINUE\n", SKYPIAX_P_LOG);
        continue;
      }
      *frame = &tech_pvt->read_frame;
#ifdef BIGENDIAN
      if (switch_test_flag(tech_pvt, TFLAG_LINEAR)) {
        switch_swap_linear((*frame)->data, (int) (*frame)->datalen / 2);
      }
#endif
      return SWITCH_STATUS_SUCCESS;
    }

    DEBUGA_SKYPE("CHANNEL READ no TFLAG_IO\n", SKYPIAX_P_LOG);
    return SWITCH_STATUS_FALSE;

  }

  DEBUGA_SKYPE("CHANNEL READ FALSE\n", SKYPIAX_P_LOG);
  return SWITCH_STATUS_FALSE;

cng:
  data = (switch_byte_t *) tech_pvt->read_frame.data;
  data[0] = 65;
  data[1] = 0;
  tech_pvt->read_frame.datalen = 2;
  tech_pvt->read_frame.flags = SFF_CNG;
  *frame = &tech_pvt->read_frame;
  return SWITCH_STATUS_SUCCESS;

}

static switch_status_t channel_write_frame(switch_core_session_t * session,
                                           switch_frame_t * frame, switch_io_flag_t flags,
                                           int stream_id)
{
  switch_channel_t *channel = NULL;
  private_t *tech_pvt = NULL;
  unsigned int sent;

  channel = switch_core_session_get_channel(session);
  assert(channel != NULL);

  tech_pvt = switch_core_session_get_private(session);
  assert(tech_pvt != NULL);

  if (!switch_test_flag(tech_pvt, TFLAG_IO)) {
    ERRORA("CIAPA \n", SKYPIAX_P_LOG);
    return SWITCH_STATUS_FALSE;
  }
#ifdef BIGENDIAN
  if (switch_test_flag(tech_pvt, TFLAG_LINEAR)) {
    switch_swap_linear(frame->data, (int) frame->datalen / 2);
  }
#endif

  sent = frame->datalen;
#ifdef WIN32
  switch_file_write(tech_pvt->audioskypepipe[1], frame->data, &sent);
#else /* WIN32 */
  sent = write(tech_pvt->audioskypepipe[1], frame->data, sent);
#endif /* WIN32 */
  if (sent != frame->datalen && sent != -1) {
    DEBUGA_SKYPE("CLI PIPE write %d\n", SKYPIAX_P_LOG, sent);
  }

  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_answer_channel(switch_core_session_t * session)
{
  private_t *tech_pvt;
  switch_channel_t *channel = NULL;

  channel = switch_core_session_get_channel(session);
  assert(channel != NULL);

  tech_pvt = switch_core_session_get_private(session);
  assert(tech_pvt != NULL);

  DEBUGA_SKYPE("ANSWERED! \n", SKYPIAX_P_LOG);

  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_receive_message(switch_core_session_t * session,
                                               switch_core_session_message_t * msg)
{
  switch_channel_t *channel;
  private_t *tech_pvt;

  channel = switch_core_session_get_channel(session);
  assert(channel != NULL);

  tech_pvt = (private_t *) switch_core_session_get_private(session);
  assert(tech_pvt != NULL);

  switch (msg->message_id) {
  case SWITCH_MESSAGE_INDICATE_ANSWER:
    {
      DEBUGA_SKYPE("MSG_ID=%d, TO BE ANSWERED!\n", SKYPIAX_P_LOG, msg->message_id);
      channel_answer_channel(session);
    }
    break;
  default:
    {
      DEBUGA_SKYPE("MSG_ID=%d\n", SKYPIAX_P_LOG, msg->message_id);
    }
    break;
  }

  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_receive_event(switch_core_session_t * session,
                                             switch_event_t * event)
{
  struct private_object *tech_pvt = switch_core_session_get_private(session);
  char *body = switch_event_get_body(event);
  switch_assert(tech_pvt != NULL);

  if (!body) {
    body = "";
  }

  WARNINGA("event: |||%s|||\n", SKYPIAX_P_LOG, body);

  return SWITCH_STATUS_SUCCESS;
}

switch_state_handler_table_t skypiax_state_handlers = {
  /*.on_init */ channel_on_init,
  /*.on_routing */ channel_on_routing,
  /*.on_execute */ channel_on_execute,
  /*.on_hangup */ channel_on_hangup,
  /*.on_exchange_media */ channel_on_exchange_media,
  /*.on_soft_execute */ channel_on_soft_execute
};

switch_io_routines_t skypiax_io_routines = {
  /*.outgoing_channel */ channel_outgoing_channel,
  /*.read_frame */ channel_read_frame,
  /*.write_frame */ channel_write_frame,
  /*.kill_channel */ channel_kill_channel,
  /*.send_dtmf */ channel_send_dtmf,
  /*.receive_message */ channel_receive_message,
  /*.receive_event */ channel_receive_event
};

static switch_call_cause_t channel_outgoing_channel(switch_core_session_t * session,
                                                    switch_event_t * var_event,
                                                    switch_caller_profile_t *
                                                    outbound_profile,
                                                    switch_core_session_t ** new_session,
                                                    switch_memory_pool_t ** pool,
                                                    switch_originate_flag_t flags)
{
	if ((*new_session = switch_core_session_request(skypiax_endpoint_interface, SWITCH_CALL_DIRECTION_OUTBOUND, pool)) != 0) {
    private_t *tech_pvt;
    switch_channel_t *channel;
    switch_caller_profile_t *caller_profile;
    char *rdest;

    switch_core_session_add_stream(*new_session, NULL);

    if ((tech_pvt =
         (private_t *) switch_core_session_alloc(*new_session, sizeof(private_t))) != 0) {
      int found = 0;
      char interface_name[256];

      if (strlen(outbound_profile->destination_number)) {
        int i;
        char *slash;

        strncpy(interface_name, outbound_profile->destination_number, 255);
        slash = strrchr(interface_name, '/');
        *slash = '\0';

        if (strncmp("ANY", interface_name, strlen(interface_name)) == 0) {
          /* we've been asked for the "ANY" interface, let's find the first idle interface */
          DEBUGA_SKYPE("Finding one available skype interface\n", SKYPIAX_P_LOG);
          tech_pvt = find_available_skypiax_interface();
          if (tech_pvt)
            found = 1;
        }
        for (i = 0; !found && i < SKYPIAX_MAX_INTERFACES; i++) {
          /* we've been asked for a normal interface name, or we have not found idle interfaces to serve as the "ANY" interface */
          if (strlen(globals.SKYPIAX_INTERFACES[i].name)
              &&
              (strncmp
               (globals.SKYPIAX_INTERFACES[i].name, interface_name,
                strlen(interface_name)) == 0)) {
            if (strlen(globals.SKYPIAX_INTERFACES[i].session_uuid_str)) {
              DEBUGA_SKYPE
                ("globals.SKYPIAX_INTERFACES[%d].name=|||%s||| session_uuid_str=|||%s||| is BUSY\n",
                 SKYPIAX_P_LOG, i, globals.SKYPIAX_INTERFACES[i].name,
                 globals.SKYPIAX_INTERFACES[i].session_uuid_str);
              switch_core_session_destroy(new_session);
              return SWITCH_CAUSE_NORMAL_TEMPORARY_FAILURE;
            }

            DEBUGA_SKYPE("globals.SKYPIAX_INTERFACES[%d].name=|||%s|||?\n", SKYPIAX_P_LOG,
                         i, globals.SKYPIAX_INTERFACES[i].name);
            tech_pvt = &globals.SKYPIAX_INTERFACES[i];
            found = 1;
            break;
          }

        }

      } else {
        ERRORA("Doh! no destination number?\n", SKYPIAX_P_LOG);
        switch_core_session_destroy(new_session);
        return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
      }

      if (!found) {
        ERRORA("Doh! no matching interface for |||%s|||?\n", SKYPIAX_P_LOG,
               interface_name);
        switch_core_session_destroy(new_session);
        return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;

      }
      channel = switch_core_session_get_channel(*new_session);
      skypiax_tech_init(tech_pvt, *new_session);
    } else {
      ERRORA("Hey where is my memory pool?\n", SKYPIAX_P_LOG);
      switch_core_session_destroy(new_session);
      return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
    }

    if (outbound_profile) {
      char name[128];

      snprintf(name, sizeof(name), "skypiax/%s", outbound_profile->destination_number);
      //snprintf(name, sizeof(name), "skypiax/%s", tech_pvt->name);
      switch_channel_set_name(channel, name);
      caller_profile = switch_caller_profile_clone(*new_session, outbound_profile);
      switch_channel_set_caller_profile(channel, caller_profile);
      tech_pvt->caller_profile = caller_profile;
    } else {
      ERRORA("Doh! no caller profile\n", SKYPIAX_P_LOG);
      switch_core_session_destroy(new_session);
      return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
    }

    rdest = strchr(caller_profile->destination_number, '/');
    *rdest++ = '\0';

    skypiax_call(tech_pvt, rdest, 30);

    switch_copy_string(tech_pvt->session_uuid_str,
                       switch_core_session_get_uuid(*new_session),
                       sizeof(tech_pvt->session_uuid_str));
    caller_profile = tech_pvt->caller_profile;
    caller_profile->destination_number = rdest;

    switch_channel_set_flag(channel, CF_OUTBOUND);
    switch_set_flag_locked(tech_pvt, TFLAG_OUTBOUND);
    switch_channel_set_state(channel, CS_INIT);
    return SWITCH_CAUSE_SUCCESS;
  }

  return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
}

/*!
 * \brief This thread runs during a call, and monitor the interface for signaling, like hangup, caller id, etc most of signaling is handled inside the skypiax_signaling_read function
 *
 */
static void *SWITCH_THREAD_FUNC skypiax_signaling_thread_func(switch_thread_t * thread,
                                                              void *obj)
{
  private_t *tech_pvt = obj;
  int res;
  int forever = 1;

  DEBUGA_SKYPE("In skypiax_signaling_thread_func: started, p=%p\n", SKYPIAX_P_LOG,
               (void *) tech_pvt);

  while (forever) {
    if (!running)
      break;
    res = skypiax_signaling_read(tech_pvt);
    if (res == CALLFLOW_INCOMING_HANGUP) {
      switch_core_session_t *session = NULL;
      //private_t *tech_pvt = NULL;
      switch_channel_t *channel = NULL;

      DEBUGA_SKYPE("skype call ended\n", SKYPIAX_P_LOG);

      if (tech_pvt) {
        session = switch_core_session_locate(tech_pvt->session_uuid_str);

        if (session) {
          channel = switch_core_session_get_channel(session);
          if (channel) {
            switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
            switch_core_session_rwunlock(session);
          } else {
            ERRORA("no channel?\n", SKYPIAX_P_LOG);
            switch_core_session_rwunlock(session);
          }
        } else {
          DEBUGA_SKYPE("no session\n", SKYPIAX_P_LOG);
        }
      } else {
        ERRORA("no tech_pvt?\n", SKYPIAX_P_LOG);
      }
      tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
      memset(tech_pvt->session_uuid_str, '\0', sizeof(tech_pvt->session_uuid_str));
    }
  }
  return NULL;
}

static switch_status_t load_config(void)
{
  char *cf = "skypiax.conf";
  switch_xml_t cfg, xml, global_settings, param, interfaces, myinterface;
  private_t *tech_pvt = NULL;

  switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, skypiax_module_pool);
  if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
    ERRORA("open of %s failed\n", SKYPIAX_P_LOG, cf);
    running = 0;
    return SWITCH_STATUS_TERM;
  }

  if ((global_settings = switch_xml_child(cfg, "global_settings"))) {
    for (param = switch_xml_child(global_settings, "param"); param; param = param->next) {
      char *var = (char *) switch_xml_attr_soft(param, "name");
      char *val = (char *) switch_xml_attr_soft(param, "value");

      if (!strcasecmp(var, "debug")) {
        DEBUGA_SKYPE("globals.debug=%d\n", SKYPIAX_P_LOG, globals.debug);
        globals.debug = atoi(val);
        DEBUGA_SKYPE("globals.debug=%d\n", SKYPIAX_P_LOG, globals.debug);
      } else if (!strcasecmp(var, "hold-music")) {
        switch_set_string(globals.hold_music, val);
        DEBUGA_SKYPE("globals.hold_music=%s\n", SKYPIAX_P_LOG, globals.hold_music);
      } else if (!strcmp(var, "port")) {
        globals.port = atoi(val);
        DEBUGA_SKYPE("globals.port=%d\n", SKYPIAX_P_LOG, globals.port);
      } else if (!strcmp(var, "codec-master")) {
        if (!strcasecmp(val, "us")) {
          switch_set_flag(&globals, GFLAG_MY_CODEC_PREFS);
        }
        DEBUGA_SKYPE("codec-master globals.debug=%d\n", SKYPIAX_P_LOG, globals.debug);
      } else if (!strcmp(var, "dialplan")) {
        set_global_dialplan(val);
        DEBUGA_SKYPE("globals.dialplan=%s\n", SKYPIAX_P_LOG, globals.dialplan);
      } else if (!strcmp(var, "destination")) {
        set_global_destination(val);
        DEBUGA_SKYPE("globals.destination=%s\n", SKYPIAX_P_LOG, globals.destination);
      } else if (!strcmp(var, "context")) {
        set_global_context(val);
        DEBUGA_SKYPE("globals.context=%s\n", SKYPIAX_P_LOG, globals.context);
      } else if (!strcmp(var, "codec-prefs")) {
        set_global_codec_string(val);
        DEBUGA_SKYPE("globals.codec_string=%s\n", SKYPIAX_P_LOG, globals.codec_string);
        globals.codec_order_last =
          switch_separate_string(globals.codec_string, ',', globals.codec_order,
                                 SWITCH_MAX_CODECS);
      } else if (!strcmp(var, "codec-rates")) {
        set_global_codec_rates_string(val);
        DEBUGA_SKYPE("globals.codec_rates_string=%s\n", SKYPIAX_P_LOG,
                     globals.codec_rates_string);
        globals.codec_rates_last =
          switch_separate_string(globals.codec_rates_string, ',', globals.codec_rates,
                                 SWITCH_MAX_CODECS);
      }

    }
  }

  if ((interfaces = switch_xml_child(cfg, "per_interface_settings"))) {
    int i = 0;

    for (myinterface = switch_xml_child(interfaces, "interface"); myinterface;
         myinterface = myinterface->next) {
      char *id = (char *) switch_xml_attr(myinterface, "id");
      char *name = (char *) switch_xml_attr(myinterface, "name");
      char *context = "default";
      char *dialplan = "XML";
      char *destination = "5000";
      char *tonegroup = NULL;
      char *digit_timeout = NULL;
      char *max_digits = NULL;
      char *hotline = NULL;
      char *dial_regex = NULL;
      char *hold_music = NULL;
      char *fail_dial_regex = NULL;
      char *enable_callerid = "true";
      char *X11_display = NULL;
      char *tcp_cli_port = NULL;
      char *tcp_srv_port = NULL;
      char *skype_user = NULL;

      uint32_t interface_id = 0, to = 0, max = 0;

      tech_pvt = NULL;

      for (param = switch_xml_child(myinterface, "param"); param; param = param->next) {
        char *var = (char *) switch_xml_attr_soft(param, "name");
        char *val = (char *) switch_xml_attr_soft(param, "value");

        if (!strcasecmp(var, "tonegroup")) {
          tonegroup = val;
        } else if (!strcasecmp(var, "digit_timeout") || !strcasecmp(var, "digit-timeout")) {
          digit_timeout = val;
        } else if (!strcasecmp(var, "context")) {
          context = val;
        } else if (!strcasecmp(var, "dialplan")) {
          dialplan = val;
        } else if (!strcasecmp(var, "destination")) {
          destination = val;
        } else if (!strcasecmp(var, "dial-regex")) {
          dial_regex = val;
        } else if (!strcasecmp(var, "enable-callerid")) {
          enable_callerid = val;
        } else if (!strcasecmp(var, "fail-dial-regex")) {
          fail_dial_regex = val;
        } else if (!strcasecmp(var, "hold-music")) {
          hold_music = val;
        } else if (!strcasecmp(var, "skype_user")) {
          skype_user = val;
        } else if (!strcasecmp(var, "tcp_cli_port")) {
          tcp_cli_port = val;
        } else if (!strcasecmp(var, "tcp_srv_port")) {
          tcp_srv_port = val;
        } else if (!strcasecmp(var, "X11-display") || !strcasecmp(var, "X11_display")) {
          X11_display = val;
        } else if (!strcasecmp(var, "max_digits") || !strcasecmp(var, "max-digits")) {
          max_digits = val;
        } else if (!strcasecmp(var, "hotline")) {
          hotline = val;
        }

      }
      if (!skype_user) {
        ERRORA("interface missing REQUIRED param 'skype_user'\n", SKYPIAX_P_LOG);
        continue;
      }

      if (!X11_display) {
        ERRORA("interface missing REQUIRED param 'X11_display'\n", SKYPIAX_P_LOG);
        continue;
      }
      if (!tcp_cli_port) {
        ERRORA("interface missing REQUIRED param 'tcp_cli_port'\n", SKYPIAX_P_LOG);
        continue;
      }

      if (!tcp_srv_port) {
        ERRORA("interface missing REQUIRED param 'tcp_srv_port'\n", SKYPIAX_P_LOG);
        continue;
      }
      if (!id) {
        ERRORA("interface missing REQUIRED param 'id'\n", SKYPIAX_P_LOG);
        continue;
      }
      if (switch_is_number(id)) {
        interface_id = atoi(id);
        DEBUGA_SKYPE("interface_id=%d\n", SKYPIAX_P_LOG, interface_id);
      } else {
        ERRORA("interface param 'id' MUST be a number, now id='%s'\n", SKYPIAX_P_LOG, id);
        continue;
      }

      if (!name) {
        WARNINGA("interface missing param 'name', not nice, but works\n", SKYPIAX_P_LOG);
      }

      if (!tonegroup) {
        tonegroup = "us";
      }

      if (digit_timeout) {
        to = atoi(digit_timeout);
      }

      if (max_digits) {
        max = atoi(max_digits);
      }

      if (name) {
        DEBUGA_SKYPE("name=%s\n", SKYPIAX_P_LOG, name);
      }
#ifndef WIN32
      if (!XInitThreads()) {
        ERRORA("Not initialized XInitThreads!\n", SKYPIAX_P_LOG);
      } else {
        DEBUGA_SKYPE("Initialized XInitThreads!\n", SKYPIAX_P_LOG);
      }
      switch_sleep(100);
#endif /* WIN32 */

      if (interface_id && interface_id < SKYPIAX_MAX_INTERFACES) {
        private_t newconf;
        switch_threadattr_t *skypiax_api_thread_attr = NULL;
        switch_threadattr_t *skypiax_signaling_thread_attr = NULL;

        memset(&newconf, '\0', sizeof(newconf));
        globals.SKYPIAX_INTERFACES[interface_id] = newconf;

        tech_pvt = &globals.SKYPIAX_INTERFACES[interface_id];

        switch_set_string(globals.SKYPIAX_INTERFACES[interface_id].interface_id, id);
        if (name) {
          switch_set_string(globals.SKYPIAX_INTERFACES[interface_id].name, name);
        } else {
          switch_set_string(globals.SKYPIAX_INTERFACES[interface_id].name, "N/A");
        }
        DEBUGA_SKYPE("CONFIGURING interface_id=%d\n", SKYPIAX_P_LOG, interface_id);
#ifdef WIN32
        globals.SKYPIAX_INTERFACES[interface_id].tcp_cli_port =
          (unsigned short) atoi(tcp_cli_port);
        globals.SKYPIAX_INTERFACES[interface_id].tcp_srv_port =
          (unsigned short) atoi(tcp_srv_port);
#else /* WIN32 */
        globals.SKYPIAX_INTERFACES[interface_id].tcp_cli_port = atoi(tcp_cli_port);
        globals.SKYPIAX_INTERFACES[interface_id].tcp_srv_port = atoi(tcp_srv_port);
#endif /* WIN32 */
        switch_set_string(globals.SKYPIAX_INTERFACES[interface_id].X11_display,
                          X11_display);
        switch_set_string(globals.SKYPIAX_INTERFACES[interface_id].skype_user,
                          skype_user);
        switch_set_string(globals.SKYPIAX_INTERFACES[interface_id].context, context);
        switch_set_string(globals.SKYPIAX_INTERFACES[interface_id].dialplan, dialplan);
        switch_set_string(globals.SKYPIAX_INTERFACES[interface_id].destination,
                          destination);
        switch_set_string(globals.SKYPIAX_INTERFACES[interface_id].context, context);

        DEBUGA_SKYPE
          ("interface_id=%d globals.SKYPIAX_INTERFACES[interface_id].X11_display=%s\n",
           SKYPIAX_P_LOG, interface_id,
           globals.SKYPIAX_INTERFACES[interface_id].X11_display);
        DEBUGA_SKYPE
          ("interface_id=%d globals.SKYPIAX_INTERFACES[interface_id].skype_user=%s\n",
           SKYPIAX_P_LOG, interface_id,
           globals.SKYPIAX_INTERFACES[interface_id].skype_user);
        DEBUGA_SKYPE
          ("interface_id=%d globals.SKYPIAX_INTERFACES[interface_id].tcp_cli_port=%d\n",
           SKYPIAX_P_LOG, interface_id,
           globals.SKYPIAX_INTERFACES[interface_id].tcp_cli_port);
        DEBUGA_SKYPE
          ("interface_id=%d globals.SKYPIAX_INTERFACES[interface_id].tcp_srv_port=%d\n",
           SKYPIAX_P_LOG, interface_id,
           globals.SKYPIAX_INTERFACES[interface_id].tcp_srv_port);
        DEBUGA_SKYPE("interface_id=%d globals.SKYPIAX_INTERFACES[interface_id].name=%s\n",
                     SKYPIAX_P_LOG, interface_id,
                     globals.SKYPIAX_INTERFACES[interface_id].name);
        DEBUGA_SKYPE
          ("interface_id=%d globals.SKYPIAX_INTERFACES[interface_id].context=%s\n",
           SKYPIAX_P_LOG, interface_id, globals.SKYPIAX_INTERFACES[interface_id].context);
        DEBUGA_SKYPE
          ("interface_id=%d globals.SKYPIAX_INTERFACES[interface_id].dialplan=%s\n",
           SKYPIAX_P_LOG, interface_id,
           globals.SKYPIAX_INTERFACES[interface_id].dialplan);
        DEBUGA_SKYPE
          ("interface_id=%d globals.SKYPIAX_INTERFACES[interface_id].destination=%s\n",
           SKYPIAX_P_LOG, interface_id,
           globals.SKYPIAX_INTERFACES[interface_id].destination);
        DEBUGA_SKYPE
          ("interface_id=%d globals.SKYPIAX_INTERFACES[interface_id].context=%s\n",
           SKYPIAX_P_LOG, interface_id, globals.SKYPIAX_INTERFACES[interface_id].context);
        WARNINGA("STARTING interface_id=%d\n", SKYPIAX_P_LOG, interface_id);

        switch_threadattr_create(&skypiax_api_thread_attr, skypiax_module_pool);
        switch_threadattr_stacksize_set(skypiax_api_thread_attr, SWITCH_THREAD_STACKSIZE);
        switch_thread_create(&globals.SKYPIAX_INTERFACES[interface_id].skypiax_api_thread,
                             skypiax_api_thread_attr, skypiax_do_skypeapi_thread,
                             &globals.SKYPIAX_INTERFACES[interface_id],
                             skypiax_module_pool);

        switch_sleep(100000);

        switch_threadattr_create(&skypiax_signaling_thread_attr, skypiax_module_pool);
        switch_threadattr_stacksize_set(skypiax_signaling_thread_attr,
                                        SWITCH_THREAD_STACKSIZE);
        switch_thread_create(&globals.SKYPIAX_INTERFACES[interface_id].
                             skypiax_signaling_thread, skypiax_signaling_thread_attr,
                             skypiax_signaling_thread_func,
                             &globals.SKYPIAX_INTERFACES[interface_id],
                             skypiax_module_pool);

        switch_sleep(100000);

        skypiax_audio_init(&globals.SKYPIAX_INTERFACES[interface_id]);

        NOTICA("WAITING roughly 10/15 seconds to find a running Skype client and connect to its SKYPE API for interface_id=%d. NB: on XP will wait for much much more! (on Vista and Linux is OK)\n", SKYPIAX_P_LOG, interface_id);
        i = 0;
        while (globals.SKYPIAX_INTERFACES[interface_id].SkypiaxHandles.api_connected == 0 && running && i < 10000) {    // 400 x 1000 is 6sec on windows XP but only 0.4 seconds on Vista  FIXME
          //DEBUGA_SKYPE("interface_id=%d, times=%d\n", SKYPIAX_P_LOG, interface_id, i);
          switch_sleep(1000);
          i++;
        }
        if (globals.SKYPIAX_INTERFACES[interface_id].SkypiaxHandles.api_connected) {
          NOTICA("Found a running Skype client, connected to its SKYPE API for interface_id=%d, waiting roughly 60/90 seconds for CURRENTUSERHANDLE==%s. NB: on XP will wait for much much more! (on Vista and Linux is OK)\n", SKYPIAX_P_LOG, interface_id, globals.SKYPIAX_INTERFACES[interface_id].skype_user);
        } else {
          ERRORA("Failed to connect to a SKYPE API for interface_id=%d, no SKYPE client running, please (re)start Skype client. Skypiax exiting\n", SKYPIAX_P_LOG, interface_id);
          running = 0;
          return SWITCH_STATUS_FALSE;
        }

        i = 0;
        while (globals.SKYPIAX_INTERFACES[interface_id].SkypiaxHandles.currentuserhandle == 0 && running && i < 60000) {    // 4000 * 1000 is 60sec on windows XP but 4seconds on Vista  FIXME
          //DEBUGA_SKYPE("interface_id=%d, times=%d\n", SKYPIAX_P_LOG, interface_id, i);
          switch_sleep(1000);
          i++;
        }
        if (globals.SKYPIAX_INTERFACES[interface_id].SkypiaxHandles.currentuserhandle) {
          WARNINGA("Interface_id=%d is now STARTED, the Skype client to which we are connected gave us the correct CURRENTUSERHANDLE (%s)\n", SKYPIAX_P_LOG, interface_id, globals.SKYPIAX_INTERFACES[interface_id].skype_user);
        } else {
          ERRORA("The Skype client to which we are connected FAILED to gave us CURRENTUSERHANDLE=%s, interface_id=%d FAILED to start. No Skype client logged in as '%s' has been found. Please (re)launch a Skype client logged in as '%s'. Skypiax exiting now\n", SKYPIAX_P_LOG, globals.SKYPIAX_INTERFACES[interface_id].skype_user, interface_id, globals.SKYPIAX_INTERFACES[interface_id].skype_user, globals.SKYPIAX_INTERFACES[interface_id].skype_user);
          running = 0;
          return SWITCH_STATUS_FALSE;
        }

      } else {
        ERRORA("interface id %d is higher than SKYPIAX_MAX_INTERFACES (%d)\n",
               SKYPIAX_P_LOG, interface_id, SKYPIAX_MAX_INTERFACES);
        continue;
      }

    }

    for (i = 0; i < SKYPIAX_MAX_INTERFACES; i++) {
      if (strlen(globals.SKYPIAX_INTERFACES[i].name)) {
        tech_pvt = &globals.SKYPIAX_INTERFACES[i];

        DEBUGA_SKYPE("i=%d globals.SKYPIAX_INTERFACES[%d].interface_id=%s\n",
                     SKYPIAX_P_LOG, i, i, globals.SKYPIAX_INTERFACES[i].interface_id);
        DEBUGA_SKYPE("i=%d globals.SKYPIAX_INTERFACES[%d].X11_display=%s\n",
                     SKYPIAX_P_LOG, i, i, globals.SKYPIAX_INTERFACES[i].X11_display);
        DEBUGA_SKYPE("i=%d globals.SKYPIAX_INTERFACES[%d].name=%s\n", SKYPIAX_P_LOG, i, i,
                     globals.SKYPIAX_INTERFACES[i].name);
        DEBUGA_SKYPE("i=%d globals.SKYPIAX_INTERFACES[%d].context=%s\n", SKYPIAX_P_LOG, i,
                     i, globals.SKYPIAX_INTERFACES[i].context);
        DEBUGA_SKYPE("i=%d globals.SKYPIAX_INTERFACES[%d].dialplan=%s\n", SKYPIAX_P_LOG,
                     i, i, globals.SKYPIAX_INTERFACES[i].dialplan);
        DEBUGA_SKYPE("i=%d globals.SKYPIAX_INTERFACES[%d].destination=%s\n",
                     SKYPIAX_P_LOG, i, i, globals.SKYPIAX_INTERFACES[i].destination);
        DEBUGA_SKYPE("i=%d globals.SKYPIAX_INTERFACES[%d].context=%s\n", SKYPIAX_P_LOG, i,
                     i, globals.SKYPIAX_INTERFACES[i].context);
      }
    }
  }

  switch_xml_free(xml);

  return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_skypiax_load)
{
  skypiax_module_pool = pool;

  memset(&globals, '\0', sizeof(globals));

  running = 1;

  if (load_config() != SWITCH_STATUS_SUCCESS) {
    running = 0;
    return SWITCH_STATUS_FALSE;
  }

  *module_interface = switch_loadable_module_create_module_interface(pool, modname);
  skypiax_endpoint_interface =
    switch_loadable_module_create_interface(*module_interface, SWITCH_ENDPOINT_INTERFACE);
  skypiax_endpoint_interface->interface_name = "skypiax";
  skypiax_endpoint_interface->io_routines = &skypiax_io_routines;
  skypiax_endpoint_interface->state_handler = &skypiax_state_handlers;

  if (running)
    /* indicate that the module should continue to be loaded */
    return SWITCH_STATUS_SUCCESS;
  else
    return SWITCH_STATUS_FALSE;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_skypiax_shutdown)
{
  int x = 100;
  private_t *tech_pvt = NULL;
  switch_status_t status;
  unsigned int howmany = 8;
  int interface_id;

  running = 0;

  for (interface_id = 0; interface_id < SKYPIAX_MAX_INTERFACES; interface_id++) {
    tech_pvt = &globals.SKYPIAX_INTERFACES[interface_id];

    if (globals.SKYPIAX_INTERFACES[interface_id].skypiax_signaling_thread) {
#ifdef WIN32
      switch_file_write(tech_pvt->SkypiaxHandles.fdesc[1], "sciutati", &howmany);   // let's the controldev_thread die
#else /* WIN32 */
      howmany = write(tech_pvt->SkypiaxHandles.fdesc[1], "sciutati", howmany);
#endif /* WIN32 */
    }

    if (globals.SKYPIAX_INTERFACES[interface_id].skypiax_api_thread) {
#ifdef WIN32
      if (SendMessage(tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle, WM_DESTROY, 0, 0) == FALSE) {  // let's the skypiax_api_thread_func die
        DEBUGA_SKYPE
          ("got FALSE here, thread probably was already dead. GetLastError returned: %d\n",
           SKYPIAX_P_LOG, GetLastError());
            globals.SKYPIAX_INTERFACES[interface_id].skypiax_api_thread=NULL;
      }
#else
      XEvent e;
      Atom atom1 =
        XInternAtom(tech_pvt->SkypiaxHandles.disp, "SKYPECONTROLAPI_MESSAGE_BEGIN",
                    False);
      memset(&e, 0, sizeof(e));
      e.xclient.type = ClientMessage;
      e.xclient.message_type = atom1;   /*  leading message */
      e.xclient.display = tech_pvt->SkypiaxHandles.disp;
      e.xclient.window = tech_pvt->SkypiaxHandles.skype_win;
      e.xclient.format = 8;

      XSendEvent(tech_pvt->SkypiaxHandles.disp, tech_pvt->SkypiaxHandles.win, False, 0,
                 &e);
      XSync(tech_pvt->SkypiaxHandles.disp, False);
#endif
    }
    while (x) {//FIXME 2 seconds?
      x--;
      switch_yield(20000); 
    }
    if (globals.SKYPIAX_INTERFACES[interface_id].skypiax_signaling_thread) {
      switch_thread_join(&status,
                         globals.SKYPIAX_INTERFACES[interface_id].
                         skypiax_signaling_thread);
    }
    if (globals.SKYPIAX_INTERFACES[interface_id].skypiax_api_thread) {
      switch_thread_join(&status,
                         globals.SKYPIAX_INTERFACES[interface_id].skypiax_api_thread);
    }
  }

  switch_safe_free(globals.dialplan);
  switch_safe_free(globals.context);
  switch_safe_free(globals.destination);
  switch_safe_free(globals.codec_string);
  switch_safe_free(globals.codec_rates_string);

  return SWITCH_STATUS_SUCCESS;
}

void *SWITCH_THREAD_FUNC skypiax_do_tcp_srv_thread(switch_thread_t * thread, void *obj)
{
  return skypiax_do_tcp_srv_thread_func(obj);
}
void *SWITCH_THREAD_FUNC skypiax_do_tcp_cli_thread(switch_thread_t * thread, void *obj)
{
  return skypiax_do_tcp_cli_thread_func(obj);
}
void *SWITCH_THREAD_FUNC skypiax_do_skypeapi_thread(switch_thread_t * thread, void *obj)
{
  return skypiax_do_skypeapi_thread_func(obj);
}

int dtmf_received(private_t * tech_pvt, char *value)
{
  switch_core_session_t *session = NULL;
  switch_channel_t *channel = NULL;

  session = switch_core_session_locate(tech_pvt->session_uuid_str);
  channel = switch_core_session_get_channel(session);

  if (channel) {
    switch_dtmf_t dtmf = { (char) value[0], switch_core_default_dtmf_duration(0) };
    DEBUGA_SKYPE("received DTMF %c on channel %s\n", SKYPIAX_P_LOG, dtmf.digit,
                 switch_channel_get_name(channel));
    switch_mutex_lock(tech_pvt->flag_mutex);
    //FIXME: why sometimes DTMFs from here do not seems to be get by FS?
    switch_channel_queue_dtmf(channel, &dtmf);
    switch_set_flag(tech_pvt, TFLAG_DTMF);
    switch_mutex_unlock(tech_pvt->flag_mutex);
  } else {
    WARNINGA("received %c DTMF, but no channel?\n", SKYPIAX_P_LOG, value[0]);
  }
  switch_core_session_rwunlock(session);

  return 0;
}

int start_audio_threads(private_t * tech_pvt)
{
  switch_threadattr_t *thd_attr = NULL;

  switch_threadattr_create(&thd_attr, skypiax_module_pool);
  switch_threadattr_detach_set(thd_attr, 1);
  switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
  switch_thread_create(&tech_pvt->tcp_srv_thread, thd_attr, skypiax_do_tcp_srv_thread,
                       tech_pvt, skypiax_module_pool);
  DEBUGA_SKYPE("started tcp_srv_thread thread.\n", SKYPIAX_P_LOG);

  switch_threadattr_create(&thd_attr, skypiax_module_pool);
  switch_threadattr_detach_set(thd_attr, 1);
  switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
  switch_thread_create(&tech_pvt->tcp_cli_thread, thd_attr, skypiax_do_tcp_cli_thread,
                       tech_pvt, skypiax_module_pool);
  DEBUGA_SKYPE("started tcp_cli_thread thread.\n", SKYPIAX_P_LOG);
  switch_sleep(100000);

  return 0;
}

int new_inbound_channel(private_t * tech_pvt)
{
  switch_core_session_t *session = NULL;
  switch_channel_t *channel = NULL;

  if ((session = switch_core_session_request(skypiax_endpoint_interface, SWITCH_CALL_DIRECTION_INBOUND, NULL)) != 0) {
    switch_core_session_add_stream(session, NULL);
    channel = switch_core_session_get_channel(session);
    skypiax_tech_init(tech_pvt, session);

    if ((tech_pvt->caller_profile =
         switch_caller_profile_new(switch_core_session_get_pool(session), "skypiax",
                                   tech_pvt->dialplan, tech_pvt->callid_name,
                                   tech_pvt->callid_number, NULL, NULL, NULL, NULL,
                                   "mod_skypiax", tech_pvt->context,
                                   tech_pvt->destination)) != 0) {
      char name[128];
      //switch_snprintf(name, sizeof(name), "skypiax/%s/%s", tech_pvt->name, tech_pvt->caller_profile->destination_number);
      switch_snprintf(name, sizeof(name), "skypiax/%s", tech_pvt->name);
      switch_channel_set_name(channel, name);
      switch_channel_set_caller_profile(channel, tech_pvt->caller_profile);
    }
    switch_channel_set_state(channel, CS_INIT);
    if (switch_core_session_thread_launch(session) != SWITCH_STATUS_SUCCESS) {
      ERRORA("Error spawning thread\n", SKYPIAX_P_LOG);
      switch_core_session_destroy(&session);
    }
  }
  switch_channel_mark_answered(channel);

  DEBUGA_SKYPE("Here\n", SKYPIAX_P_LOG);

  return 0;
}

int remote_party_is_ringing(private_t * tech_pvt)
{
  switch_core_session_t *session = NULL;
  switch_channel_t *channel = NULL;

  if (strlen(tech_pvt->session_uuid_str)) {
    session = switch_core_session_locate(tech_pvt->session_uuid_str);
  } else {
    ERRORA("No session???\n", SKYPIAX_P_LOG);
  }
  if (session) {
    channel = switch_core_session_get_channel(session);
  } else {
    ERRORA("No session???\n", SKYPIAX_P_LOG);
  }
  if (channel) {
    switch_channel_mark_ring_ready(channel);
    DEBUGA_SKYPE("skype_call: REMOTE PARTY RINGING\n", SKYPIAX_P_LOG);
  } else {
    ERRORA("No channel???\n", SKYPIAX_P_LOG);
  }

  switch_core_session_rwunlock(session);

  return 0;
}


int remote_party_is_early_media(private_t * tech_pvt)
{
  switch_core_session_t *session = NULL;
  switch_channel_t *channel = NULL;

  if (strlen(tech_pvt->session_uuid_str)) {
    session = switch_core_session_locate(tech_pvt->session_uuid_str);
  } else {
    ERRORA("No session???\n", SKYPIAX_P_LOG);
  }
  if (session) {
    channel = switch_core_session_get_channel(session);
    switch_core_session_add_stream(session, NULL);
  } else {
    ERRORA("No session???\n", SKYPIAX_P_LOG);
  }
  if (channel) {
    //switch_channel_mark_pre_answered(channel);
    NOTICA("skype_call: REMOTE PARTY EARLY MEDIA, we will pass you the audio, just the code is not yet written :-)\n", SKYPIAX_P_LOG);
  } else {
    ERRORA("No channel???\n", SKYPIAX_P_LOG);
  }

  switch_core_session_rwunlock(session);

  return 0;
}

int outbound_channel_answered(private_t * tech_pvt)
{
  switch_core_session_t *session = NULL;
  switch_channel_t *channel = NULL;

  if (strlen(tech_pvt->session_uuid_str)) {
    session = switch_core_session_locate(tech_pvt->session_uuid_str);
  } else {
    ERRORA("No session???\n", SKYPIAX_P_LOG);
  }
  if (session) {
    channel = switch_core_session_get_channel(session);
  } else {
    ERRORA("No session???\n", SKYPIAX_P_LOG);
  }
  if (channel) {
    switch_channel_mark_answered(channel);
    //DEBUGA_SKYPE("skype_call: %s, answered\n", SKYPIAX_P_LOG, id);
  } else {
    ERRORA("No channel???\n", SKYPIAX_P_LOG);
  }

  switch_core_session_rwunlock(session);
  DEBUGA_SKYPE("HERE!\n", SKYPIAX_P_LOG);

  return 0;
}

private_t *find_available_skypiax_interface(void)
{
  private_t *tech_pvt = NULL;
  int found = 0;
  int i;

  for (i = 0; !found && i < SKYPIAX_MAX_INTERFACES; i++) {
    if (strlen(globals.SKYPIAX_INTERFACES[i].name)) {
      int skype_state = 0;

      tech_pvt = &globals.SKYPIAX_INTERFACES[i];
      skype_state = tech_pvt->interface_state;
      DEBUGA_SKYPE("skype interface: %d, name: %s, state: %d\n", SKYPIAX_P_LOG, i,
                   globals.SKYPIAX_INTERFACES[i].name, skype_state);
      if (SKYPIAX_STATE_DOWN == skype_state || 0 == skype_state) {
        found = 1;
        break;
      }
    }
  }

  if (found)
    return tech_pvt;
  else
    return NULL;
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
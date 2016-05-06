/*
     This file is part of GNUnet.
     Copyright (C) 2016 GNUnet e.V.

     GNUnet is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published
     by the Free Software Foundation; either version 3, or (at your
     option) any later version.

     GNUnet is distributed in the hope that it will be useful, but
     WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with GNUnet; see the file COPYING.  If not, write to the
     Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
     Boston, MA 02110-1301, USA.
*/

/**
 * CLI tool to interact with the social service.
 *
 * @author Gabor X Toth
 */

#include <inttypes.h>

#include "platform.h"
#include "gnunet_util_lib.h"
#include "gnunet_social_service.h"
#include "gnunet_core_service.h"

#define TIMEOUT GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30)

#define DATA2ARG(data) data, sizeof (data)

/* operations corresponding to API calls */

/** --host-enter */
static int op_host_enter;

/** --host-leave */
static int op_host_leave;

/** --host-announce */
static int op_host_announce;

/** --guest-enter */
static int op_guest_enter;

/** --guest-leave */
static int op_guest_leave;

/** --guest-talk */
static int op_guest_talk;

/** --history-replay */
static char *op_history_replay;

/** --history-replay-latest */
static char *op_history_replay_latest;

/** --look-at */
static int op_look_at;

/** --look-for */
static int op_look_for;


/* options */

/** --app */
static char *opt_app = "cli";

/** --place */
static char *opt_place;

/** --ego */
static char *opt_ego;

/** --peer */
static char *opt_peer;

/** --follow */
static int opt_follow;

/** --method */
static char *opt_method;

/** --data */
// FIXME: could also come from STDIN
static char *opt_data;

/** --name */
static char *opt_name;

/** --start */
static uint64_t opt_start;

/** --end */
static uint64_t opt_end;

/** --limit */
static int opt_limit;


/* global vars */

/** exit code */
static int ret = 1;

/** Task handle for timeout termination. */
struct GNUNET_SCHEDULER_Task *timeout_task;

const struct GNUNET_CONFIGURATION_Handle *cfg;

struct GNUNET_CORE_Handle *core;
struct GNUNET_PeerIdentity peer;

struct GNUNET_SOCIAL_App *app;

/** public key of connected place */
struct GNUNET_CRYPTO_EddsaPublicKey place_pub_key;

/** hash of @a place_pub_key */
struct GNUNET_HashCode place_pub_hash;

struct GNUNET_PSYC_Slicer *slicer;

struct GNUNET_SOCIAL_Ego *ego;
struct GNUNET_CRYPTO_EcdsaPublicKey ego_pub_key;

struct GNUNET_SOCIAL_Host *hst;
struct GNUNET_SOCIAL_Guest *gst;
struct GNUNET_SOCIAL_Place *plc;


/* DISCONNECT */


static void
disconnect ()
{
  GNUNET_SOCIAL_app_disconnect (app);
  GNUNET_CORE_disconnect (core);
  GNUNET_SCHEDULER_shutdown ();
}

/**
 * Terminate the test case (failure).
 *
 * @param cls NULL
 */
static void
timeout (void *cls)
{
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "Timeout.\n");
  disconnect ();
}

static void
schedule_end (void *cls)
{
  ret = 0;
  disconnect ();
}


static void
end ()
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "The end.\n");

  if (timeout_task != NULL)
  {
    GNUNET_SCHEDULER_cancel (timeout_task);
    timeout_task = NULL;
  }
  GNUNET_SCHEDULER_add_now (&schedule_end, NULL);
}


/* LEAVE */


static void
host_left ()
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "The host has left the place.\n");
  end ();
}


static void
host_leave ()
{
  GNUNET_SOCIAL_host_leave (hst, NULL, &host_left, NULL);
  hst = NULL;
  plc = NULL;
}


static void
guest_left (void *cls)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "The guest has left the place.\n");
}


static void
guest_leave ()
{
  struct GNUNET_PSYC_Environment *env = GNUNET_PSYC_env_create ();
  GNUNET_PSYC_env_add (env, GNUNET_PSYC_OP_SET,
                       "_notice_place_leave", DATA2ARG ("Leaving."));
  GNUNET_SOCIAL_guest_leave (gst, env, &guest_left, NULL);
  GNUNET_PSYC_env_destroy (env);
  gst = NULL;
  plc = NULL;
}


/* ANNOUNCE / TALK */


struct TransmitClosure
{
  const char *data;
  size_t size;
} tmit;


static int
notify_data (void *cls, uint16_t *data_size, void *data)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Transmit notify data: %u bytes available\n",
              *data_size);

  struct TransmitClosure *tmit = cls;
  uint16_t size = tmit->size < *data_size ? tmit->size : *data_size;
  *data_size = size;
  memcpy (data, tmit->data, size);

  tmit->size -= size;
  tmit->data += size;

  return 0 == tmit->size ? GNUNET_NO : GNUNET_YES;
}


static void
host_announce (const char *method, const char *data, size_t data_size)
{
  struct GNUNET_PSYC_Environment *env = GNUNET_PSYC_env_create ();
  GNUNET_PSYC_env_add (env, GNUNET_PSYC_OP_SET,
                       "_foo", DATA2ARG ("bar baz"));

  tmit = (struct TransmitClosure) {};
  tmit.data = data;
  tmit.size = data_size;

  GNUNET_SOCIAL_host_announce (hst, method, env,
                               &notify_data, &tmit,
                               GNUNET_SOCIAL_ANNOUNCE_NONE);
}


static void
guest_talk (const char *method,
            const char *data, size_t data_size)
{
  struct GNUNET_PSYC_Environment *env = GNUNET_PSYC_env_create ();
  GNUNET_PSYC_env_add (env, GNUNET_PSYC_OP_SET,
                       "_foo", DATA2ARG ("bar baz"));

  tmit = (struct TransmitClosure) {};
  tmit.data = data;
  tmit.size = data_size;

  GNUNET_SOCIAL_guest_talk (gst, method, env,
                            &notify_data, &tmit,
                            GNUNET_SOCIAL_TALK_NONE);
}


/* HISTORY REPLAY */


static void
recv_history_replay_result (void *cls, int64_t result,
                            const void *data, uint16_t data_size)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Guest received history replay result: %" PRId64 "\n"
              "%.*s\n",
              result, data_size, (const char *) data);
}


static void
history_replay (uint64_t start, uint64_t end, const char *prefix)
{
  GNUNET_SOCIAL_place_history_replay (plc, start, end, prefix,
                                      GNUNET_PSYC_HISTORY_REPLAY_LOCAL,
                                      slicer,
                                      &recv_history_replay_result,
                                      NULL);
}


static void
history_replay_latest (uint64_t limit, const char *prefix)
{
  GNUNET_SOCIAL_place_history_replay_latest (plc, limit, prefix,
                                             GNUNET_PSYC_HISTORY_REPLAY_LOCAL,
                                             slicer,
                                             &recv_history_replay_result,
                                             NULL);
}


/* LOOK AT/FOR */


static void
look_result (void *cls, int64_t result_code,
             const void *data, uint16_t data_size)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "look_result: %" PRId64 "\n", result_code);

}


static void
look_var (void *cls,
          const struct GNUNET_MessageHeader *mod,
          const char *name,
          const void *value,
          uint32_t value_size,
          uint32_t full_value_size)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "guest_look_at_var: %s\n%.*s\n",
              name, value_size, (const char *) value);
}


static void
look_at (const char *name)
{
  GNUNET_SOCIAL_place_look_at (plc, name, look_var, look_result, NULL);
}


static void
look_for (const char *name)
{
  GNUNET_SOCIAL_place_look_for (plc, name, look_var, look_result, NULL);
}


/* SLICER */


static void
slicer_recv_method (void *cls,
                    const struct GNUNET_PSYC_MessageHeader *msg,
                    const struct GNUNET_PSYC_MessageMethod *meth,
                    uint64_t message_id,
                    const char *method_name)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Received method for message ID %" PRIu64 ":\n"
              "%s (flags: %x)\n",
              message_id, method_name, ntohl (meth->flags));
}


static void
slicer_recv_modifier (void *cls,
                      const struct GNUNET_PSYC_MessageHeader *msg,
                      const struct GNUNET_MessageHeader *pmsg,
                      uint64_t message_id,
                      enum GNUNET_PSYC_Operator oper,
                      const char *name,
                      const void *value,
                      uint16_t value_size,
                      uint16_t full_value_size)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Received modifier for message ID %" PRIu64 ":\n"
              "%c%s: %.*s (size: %u)\n",
              message_id, oper, name, value_size, (const char *) value, value_size);
}


static void
slicer_recv_data (void *cls,
                  const struct GNUNET_PSYC_MessageHeader *msg,
                  const struct GNUNET_MessageHeader *pmsg,
                  uint64_t message_id,
                  const void *data,
                  uint16_t data_size)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Received data for message ID %" PRIu64 ":\n"
              "%.*s\n",
              message_id, data_size, (const char *) data);
}


static void
slicer_recv_eom (void *cls,
                const struct GNUNET_PSYC_MessageHeader *msg,
                const struct GNUNET_MessageHeader *pmsg,
                uint64_t message_id,
                uint8_t is_cancelled)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Received end of message ID %" PRIu64
              ", cancelled: %u\n",
              message_id, is_cancelled);
}


static struct GNUNET_PSYC_Slicer *
slicer_create ()
{
  slicer = GNUNET_PSYC_slicer_create ();

  /* register slicer to receive incoming messages with any method name */
  GNUNET_PSYC_slicer_method_add (slicer, "", NULL,
                                 slicer_recv_method, slicer_recv_modifier,
                                 slicer_recv_data, slicer_recv_eom, NULL);
  return slicer;
}


/* GUEST ENTER */


static void
guest_recv_entry_decision (void *cls,
                           int is_admitted,
                           const struct GNUNET_PSYC_Message *entry_msg)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Guest received entry decision %d\n",
              is_admitted);

  if (NULL != entry_msg)
  {
    struct GNUNET_PSYC_Environment *env = GNUNET_PSYC_env_create ();
    const char *method_name = NULL;
    const void *data = NULL;
    uint16_t data_size = 0;
    struct GNUNET_PSYC_MessageHeader *
      pmsg = GNUNET_PSYC_message_header_create_from_psyc (entry_msg);
    GNUNET_PSYC_message_parse (pmsg, &method_name, env, &data, &data_size);
    GNUNET_free (pmsg);

    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "%s\n%.*s\n",
                method_name, data_size, (const char *) data);
  }
}


static void
guest_recv_local_enter (void *cls, int result,
                        const struct GNUNET_CRYPTO_EddsaPublicKey *place_pub_key,
                        uint64_t max_message_id)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Guest entered to local place: %d, max_message_id: %" PRIu64 "\n",
              result, max_message_id);
  GNUNET_assert (0 <= result);
}


static struct GNUNET_PSYC_Message *
guest_enter_msg_create ()
{
  const char *method_name = "_request_enter";
  struct GNUNET_PSYC_Environment *env = GNUNET_PSYC_env_create ();
  GNUNET_PSYC_env_add (env, GNUNET_PSYC_OP_SET,
                       "_foo", DATA2ARG ("bar"));
  void *data = "let me in";
  uint16_t data_size = strlen (data) + 1;

  return GNUNET_PSYC_message_create (method_name, env, data, data_size);
}


static void
guest_enter (struct GNUNET_PeerIdentity *peer)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Entering to place as guest.\n");

  gst = GNUNET_SOCIAL_guest_enter (app, ego, &place_pub_key,
                                   GNUNET_PSYC_SLAVE_JOIN_NONE,
                                   peer, 0, NULL, guest_enter_msg_create (),
                                   slicer_create (),
                                   guest_recv_local_enter,
                                   guest_recv_entry_decision, NULL);
  plc = GNUNET_SOCIAL_guest_get_place (gst);
}


static void
guest_enter_by_name (const char *gns_name)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Entering to place by name as guest.\n");

  gst = GNUNET_SOCIAL_guest_enter_by_name (app, ego, gns_name, NULL,
                                           guest_enter_msg_create (), slicer,
                                           guest_recv_local_enter,
                                           guest_recv_entry_decision, NULL);
  plc = GNUNET_SOCIAL_guest_get_place (gst);
}


/* HOST ENTER */


static void
host_answer_door (void *cls,
                  struct GNUNET_SOCIAL_Nym *nym,
                  const char *method_name,
                  struct GNUNET_PSYC_Environment *env,
                  const void *data,
                  size_t data_size)
{
  const struct GNUNET_CRYPTO_EcdsaPublicKey *
    nym_key = GNUNET_SOCIAL_nym_get_pub_key (nym);
  char *
    nym_str = GNUNET_CRYPTO_ecdsa_public_key_to_string (nym_key);

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Entry request: %s\n", nym_str);
  GNUNET_free (nym_str);
}


static void
host_farewell (void *cls,
               const struct GNUNET_SOCIAL_Nym *nym,
               struct GNUNET_PSYC_Environment *env)
{
  const struct GNUNET_CRYPTO_EcdsaPublicKey *
    nym_key = GNUNET_SOCIAL_nym_get_pub_key (nym);
  char *
    nym_str = GNUNET_CRYPTO_ecdsa_public_key_to_string (nym_key);

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Farewell: %s\n", nym_str);
  GNUNET_free (nym_str);
}


static void
host_entered (void *cls, int result,
              const struct GNUNET_CRYPTO_EddsaPublicKey *pub_key,
              uint64_t max_message_id)
{
  place_pub_key = *pub_key;
  GNUNET_CRYPTO_hash (&place_pub_key, sizeof (place_pub_key), &place_pub_hash);
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Entered: %s, max_message_id: %" PRIu64 "\n",
              GNUNET_h2s_full (&place_pub_hash), max_message_id);
}


static void
host_enter ()
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "host_enter()\n");

  hst = GNUNET_SOCIAL_host_enter (app, ego,
                                  GNUNET_PSYC_CHANNEL_PRIVATE,
                                  slicer_create (), host_entered,
                                  host_answer_door, host_farewell, NULL);
  plc = GNUNET_SOCIAL_host_get_place (hst);
}


/* PLACE RECONNECT */


static void
place_reconnected ()
{
  if (op_history_replay) {
    history_replay (opt_start, opt_end, opt_method);
  }
  else if (op_history_replay_latest) {
    history_replay_latest (opt_limit, opt_method);
  }
  else if (op_look_at) {
    look_at (opt_name);
  }
  else if (op_look_for) {
    look_for (opt_name);
  }
}


static void
host_reconnected (void *cls, int result,
		  const struct GNUNET_CRYPTO_EddsaPublicKey *place_pub_key,
		  uint64_t max_message_id)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Host reconnected\n");

  if (op_host_leave) {
    host_leave ();
  }
  else if (op_host_announce) {
    host_announce (opt_method, opt_data, strlen (opt_data));
  }
  else {
    place_reconnected ();
  }
}


static void
guest_reconnected (void *cls, int result,
                   const struct GNUNET_CRYPTO_EddsaPublicKey *place_pub_key,
                   uint64_t max_message_id)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Guest reconnected\n");

  if (op_guest_leave) {
    guest_leave ();
  }
  else if (op_guest_talk) {
    guest_talk (opt_method, opt_data, strlen (opt_data));
  }
  else {
    place_reconnected ();
  }
}


/* APP */


static void
app_connected (void *cls)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "App connected: %p\n", cls);

  if (op_host_enter) {
    host_enter ();
  }
  else if (op_guest_enter) {
    guest_enter (&peer);
    // FIXME: guest_enter_by_name
  }
}


static void
app_recv_host (void *cls,
               struct GNUNET_SOCIAL_HostConnection *hconn,
               struct GNUNET_SOCIAL_Ego *ego,
               const struct GNUNET_CRYPTO_EddsaPublicKey *host_pub_key,
               enum GNUNET_SOCIAL_AppPlaceState place_state)
{
  struct GNUNET_HashCode host_pub_hash;
  GNUNET_CRYPTO_hash (host_pub_key, sizeof (*host_pub_key), &host_pub_hash);
  char *
    host_pub_str = GNUNET_CRYPTO_eddsa_public_key_to_string (host_pub_key);

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Host: %s (%s)\n",
              GNUNET_h2s_full (&host_pub_hash), host_pub_str);
  GNUNET_free (host_pub_str);

  if (0 == memcmp (&place_pub_key, host_pub_key, sizeof (*host_pub_key)))
  {
    hst = GNUNET_SOCIAL_host_enter_reconnect (hconn, slicer_create (), host_reconnected,
                                              host_answer_door, host_farewell, NULL);
    plc = GNUNET_SOCIAL_host_get_place (hst);
  }
}


static void
app_recv_guest (void *cls,
                struct GNUNET_SOCIAL_GuestConnection *gconn,
                struct GNUNET_SOCIAL_Ego *ego,
                const struct GNUNET_CRYPTO_EddsaPublicKey *guest_pub_key,
                enum GNUNET_SOCIAL_AppPlaceState place_state)
{
  struct GNUNET_HashCode guest_pub_hash;
  GNUNET_CRYPTO_hash (guest_pub_key, sizeof (*guest_pub_key), &guest_pub_hash);
  char *
    guest_pub_str = GNUNET_CRYPTO_eddsa_public_key_to_string (guest_pub_key);

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Guest: %s (%s)\n",
              GNUNET_h2s_full (&guest_pub_hash), guest_pub_str);
  GNUNET_free (guest_pub_str);

  if (0 == memcmp (&place_pub_key, guest_pub_key, sizeof (*guest_pub_key)))
  {
    gst = GNUNET_SOCIAL_guest_enter_reconnect (gconn, GNUNET_PSYC_SLAVE_JOIN_NONE,
                                               slicer_create (), guest_reconnected, NULL);
    plc = GNUNET_SOCIAL_guest_get_place (gst);
  }
}


static void
app_recv_ego (void *cls,
              struct GNUNET_SOCIAL_Ego *ego,
              const struct GNUNET_CRYPTO_EcdsaPublicKey *ego_pub_key,
              const char *name)
{
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Ego: %s\t%s\n",
              name, GNUNET_CRYPTO_ecdsa_public_key_to_string (ego_pub_key));
}


static void
app_connect ()
{
  app = GNUNET_SOCIAL_app_connect (cfg, opt_app,
                                   app_recv_ego,
                                   app_recv_host,
                                   app_recv_guest,
                                   app_connected,
                                   NULL);
}


/* CORE */


static void
core_connected (void *cls, const struct GNUNET_PeerIdentity *my_identity)
{
  peer = *my_identity;
  app_connect ();
}


/* RUN */


/**
 * Main function run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param cfg configuration
 */
static void
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *c)
{
  cfg = c;

  if (!opt_follow)
  {
    timeout_task = GNUNET_SCHEDULER_add_delayed (TIMEOUT, &timeout, NULL);
  }

  if (op_host_enter && NULL != opt_place)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                _("--place must not be specified when using --host-enter\n"));
    return;
  }

  if (!opt_place
      || GNUNET_OK != GNUNET_CRYPTO_eddsa_public_key_from_string (opt_place,
                                                                  strlen (opt_place),
                                                                  &place_pub_key))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                _("--place missing or invalid.\n"));
    return;
  }

  if (opt_ego)
  {
    GNUNET_CRYPTO_ecdsa_public_key_from_string (opt_ego,
                                                strlen (opt_ego),
                                                &ego_pub_key);
  }

  if (opt_peer)
  {
    // FIXME: peer ID from string
  }

  core = GNUNET_CORE_connect (cfg, NULL, &core_connected, NULL, NULL,
                              NULL, GNUNET_NO, NULL, GNUNET_NO, NULL);
}


/**
 * The main function to obtain peer information.
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc, char *const *argv)
{
  int res;
  static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    /* operations */

    { 'E', "host-enter", NULL,
      gettext_noop ("create a place for nyms to join"),
      GNUNET_NO, &GNUNET_GETOPT_set_one, &op_host_enter },

    { 'L', "host-leave", NULL,
      gettext_noop ("destroy a place we were hosting"),
      GNUNET_NO, &GNUNET_GETOPT_set_one, &op_host_leave },

    { 'A', "host-announce", NULL,
      gettext_noop ("publish something to a place we are hosting"),
      GNUNET_NO, &GNUNET_GETOPT_set_one, &op_host_announce },

    { 'e', "guest-enter", NULL,
      gettext_noop ("join somebody else's place"),
      GNUNET_NO, &GNUNET_GETOPT_set_one, &op_guest_enter },

    { 'l', "guest-leave", NULL,
      gettext_noop ("leave somebody else's place"),
      GNUNET_NO, &GNUNET_GETOPT_set_one, &op_guest_leave },

    { 't', "guest-talk", NULL,
      gettext_noop ("submit something to somebody's place"),
      GNUNET_NO, &GNUNET_GETOPT_set_one, &op_guest_talk },

    { 'R', "history-replay", NULL,
      gettext_noop ("replay history of messages between message IDs --start and --end"),
      GNUNET_NO, &GNUNET_GETOPT_set_one, &op_history_replay },

    { 'r', "history-replay-latest", NULL,
      gettext_noop ("replay history of latest messages up to the given --limit"),
      GNUNET_NO, &GNUNET_GETOPT_set_one, &op_history_replay_latest },

    /* options */

    { 'A', "app", "application ID",
      gettext_noop ("application ID to use when connecting"),
      GNUNET_NO, &GNUNET_GETOPT_set_string, &opt_app },

    { 'p', "place", "PUBKEY",
      gettext_noop ("public key of place"),
      GNUNET_NO, &GNUNET_GETOPT_set_string, &opt_place },

    { 'P', "peer", "PEER_ID",
      gettext_noop ("peer ID for --guest-enter"),
      GNUNET_NO, &GNUNET_GETOPT_set_string, &opt_peer },

    { 'g', "ego", "PUBKEY",
      gettext_noop ("public key of ego"),
      GNUNET_NO, &GNUNET_GETOPT_set_string, &opt_place },

    { 'f', "follow", NULL,
      gettext_noop ("wait for incoming messages"),
      GNUNET_NO, &GNUNET_GETOPT_set_one, &opt_follow },

    { 'm', "method", "METHOD_NAME",
      gettext_noop ("method name"),
      GNUNET_NO, &GNUNET_GETOPT_set_string, &opt_method },

    { 'd', "data", "DATA",
      gettext_noop ("message body to transmit"),
      GNUNET_NO, &GNUNET_GETOPT_set_string, &opt_data },

    { 'n', "name", "VAR_NAME",
      gettext_noop ("state var name to query"),
      GNUNET_NO, &GNUNET_GETOPT_set_string, &opt_name },

    { 'a', "start", NULL,
      gettext_noop ("start message ID for history replay"),
      GNUNET_NO, &GNUNET_GETOPT_set_ulong, &opt_start },

    { 'z', "end", NULL,
      gettext_noop ("end message ID for history replay"),
      GNUNET_NO, &GNUNET_GETOPT_set_ulong, &opt_end },

    { 'n', "limit", NULL,
      gettext_noop ("number of messages to replay from history"),
      GNUNET_NO, &GNUNET_GETOPT_set_ulong, &opt_limit },

    GNUNET_GETOPT_OPTION_END
  };

  if (GNUNET_OK != GNUNET_STRINGS_get_utf8_args (argc, argv, &argc, &argv))
    return 2;

  const char *help =
    _ ("interact with the social service: enter/leave, send/receive messages, access history and state");
  const char *usage =
    "gnunet-social --host-enter --ego <name or pubkey> [--listen]\n"
    "gnunet-social --host-leave --place <pubkey>\n"
    "gnunet-social --host-announce --place <pubkey> --method <method_name> --data <message body>\n"
    "\n"
    "gnunet-social --guest-enter --place <pubkey> --ego <name or pubkey> [--listen]\n"
    "gnunet-social --guest-leave --place <pubkey>\n"
    "gnunet-social --guest-talk --place <pubkey> --method <method_nmae> --data <data>\n"
    "\n"
    "gnunet-social --history-replay --place <pubkey> --start <msgid> --end <msgid>  [--method <method_prefix>]\n"
    "gnunet-social --history-replay-latest --place <pubkey> --limit <msg_limit> [--method <method_prefix>]\n"
    "\n"
    "gnunet-social --look-at --place <pubkey> --name <full_name>\n"
    "gnunet-social --look-for --place <pubkey> --name <name_prefix>\n";

  res = GNUNET_PROGRAM_run (argc, argv, usage,
                            help, options, &run, NULL);

  GNUNET_free ((void *) argv);

  if (GNUNET_OK == res)
    return ret;
  else
    return 1;
}
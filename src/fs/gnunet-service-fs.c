/*
     This file is part of GNUnet.
     (C) 2009 Christian Grothoff (and other contributing authors)

     GNUnet is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published
     by the Free Software Foundation; either version 2, or (at your
     option) any later version.

     GNUnet is distributed in the hope that it will be useful, but
     WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with GNUnet; see the file COPYING.  If not, write to the
     Free Software Foundation, Inc., 59 Temple Place - Suite 330,
     Boston, MA 02111-1307, USA.
*/

/**
 * @file fs/gnunet-service-fs.c
 * @brief program that provides the file-sharing service
 * @author Christian Grothoff
 *
 * TODO:
 * - fix gazillion of minor FIXME's
 * - possible major issue: we may queue "gazillions" of (K|S)Blocks for the
 *   core to transmit to another peer; need to make sure this is bounded overall...
 * - randomly delay processing for improved anonymity (can wait)
 * - content migration (put in local DS) (can wait)
 * - handle some special cases when forwarding replies based on tracked requests (can wait)
 * - tracking of success correlations for hot-path routing (can wait)
 * - various load-based actions (can wait)
 * - validation of KSBLOCKS (can wait)
 * - remove on-demand blocks if they keep failing (can wait)
 * - check that we decrement PIDs always where necessary (can wait)
 * - find out how to use core-pulling instead of pushing (at least for some cases)
 */
#include "platform.h"
#include <float.h>
#include "gnunet_core_service.h"
#include "gnunet_datastore_service.h"
#include "gnunet_peer_lib.h"
#include "gnunet_protocols.h"
#include "gnunet_signatures.h"
#include "gnunet_util_lib.h"
#include "gnunet-service-fs_indexing.h"
#include "fs.h"

#define DEBUG_FS GNUNET_NO

/**
 * Signature of a function that is called whenever a datastore
 * request can be processed (or an entry put on the queue times out).
 *
 * @param cls closure
 * @param ok GNUNET_OK if DS is ready, GNUNET_SYSERR on timeout
 */
typedef void (*RequestFunction)(void *cls,
				int ok);


/**
 * Doubly-linked list of our requests for the datastore.
 */
struct DatastoreRequestQueue
{

  /**
   * This is a doubly-linked list.
   */
  struct DatastoreRequestQueue *next;

  /**
   * This is a doubly-linked list.
   */
  struct DatastoreRequestQueue *prev;

  /**
   * Function to call (will issue the request).
   */
  RequestFunction req;

  /**
   * Closure for req.
   */
  void *req_cls;

  /**
   * When should this request time-out because we don't care anymore?
   */
  struct GNUNET_TIME_Absolute timeout;
    
  /**
   * ID of task used for signaling timeout.
   */
  GNUNET_SCHEDULER_TaskIdentifier task;

};


/**
 * Closure for processing START_SEARCH messages from a client.
 */
struct LocalGetContext
{

  /**
   * This is a doubly-linked list.
   */
  struct LocalGetContext *next;

  /**
   * This is a doubly-linked list.
   */
  struct LocalGetContext *prev;

  /**
   * Client that initiated the search.
   */
  struct GNUNET_SERVER_Client *client;

  /**
   * Array of results that we've already received 
   * (can be NULL).
   */
  GNUNET_HashCode *results; 

  /**
   * Bloomfilter over all results (for fast query construction);
   * NULL if we don't have any results.
   *
   * FIXME: this member is not used, is that OK? If so, it should
   * be removed!
   */
  struct GNUNET_CONTAINER_BloomFilter *results_bf; 

  /**
   * DS request associated with this operation.
   */
  struct DatastoreRequestQueue *req;

  /**
   * Current result message to transmit to client (or NULL).
   */
  struct ContentMessage *result;
  
  /**
   * Type of the content that we're looking for.
   * 0 for any.
   */
  uint32_t type;

  /**
   * Desired anonymity level.
   */
  uint32_t anonymity_level;

  /**
   * Number of results actually stored in the results array.
   */
  unsigned int results_used;
  
  /**
   * Size of the results array in memory.
   */
  unsigned int results_size;
  
  /**
   * Size (in bytes) of the 'results_bf' bloomfilter.
   *
   * FIXME: this member is not used, is that OK? If so, it should
   * be removed!
   */
  size_t results_bf_size;

  /**
   * If the request is for a DBLOCK or IBLOCK, this is the identity of
   * the peer that is known to have a response.  Set to all-zeros if
   * such a target is not known (note that even if OUR anonymity
   * level is >0 we may happen to know the responder's identity;
   * nevertheless, we should probably not use it for a DHT-lookup
   * or similar blunt actions in order to avoid exposing ourselves).
   */
  struct GNUNET_PeerIdentity target;

  /**
   * If the request is for an SBLOCK, this is the identity of the
   * pseudonym to which the SBLOCK belongs. 
   */
  GNUNET_HashCode namespace;

  /**
   * Hash of the keyword (aka query) for KBLOCKs; Hash of
   * the CHK-encoded block for DBLOCKS and IBLOCKS (aka query)
   * and hash of the identifier XORed with the target for
   * SBLOCKS (aka query).
   */
  GNUNET_HashCode query;

};


/**
 * Possible routing policies for an FS-GET request.
 */
enum RoutingPolicy
  {
    /**
     * Simply drop the request.
     */
    ROUTING_POLICY_NONE = 0,
    
    /**
     * Answer it if we can from local datastore.
     */
    ROUTING_POLICY_ANSWER = 1,

    /**
     * Forward the request to other peers (if possible).
     */
    ROUTING_POLICY_FORWARD = 2,

    /**
     * Forward to other peers, and ask them to route
     * the response via ourselves.
     */
    ROUTING_POLICY_INDIRECT = 6,
    
    /**
     * Do everything we could possibly do (that would
     * make sense).
     */
    ROUTING_POLICY_ALL = 7
  };


/**
 * Internal context we use for our initial processing
 * of a GET request.
 */
struct ProcessGetContext
{
  /**
   * The search query (used for datastore lookup).
   */
  GNUNET_HashCode query;
  
  /**
   * Which peer we should forward the response to.
   */
  struct GNUNET_PeerIdentity reply_to;

  /**
   * Namespace for the result (only set for SKS requests)
   */
  GNUNET_HashCode namespace;

  /**
   * Peer that we should forward the query to if possible
   * (since that peer likely has the content).
   */
  struct GNUNET_PeerIdentity prime_target;

  /**
   * When did we receive this request?
   */
  struct GNUNET_TIME_Absolute start_time;

  /**
   * Our entry in the DRQ (non-NULL while we wait for our
   * turn to interact with the local database).
   */
  struct DatastoreRequestQueue *drq;

  /**
   * Filter used to eliminate duplicate results.  Can be NULL if we
   * are not yet filtering any results.
   */
  struct GNUNET_CONTAINER_BloomFilter *bf;

  /**
   * Bitmap describing which of the optional
   * hash codes / peer identities were given to us.
   */
  uint32_t bm;

  /**
   * Desired block type.
   */
  uint32_t type;

  /**
   * Priority of the request.
   */
  uint32_t priority;

  /**
   * Size of the 'bf' (in bytes).
   */
  size_t bf_size;

  /**
   * In what ways are we going to process
   * the request?
   */
  enum RoutingPolicy policy;

  /**
   * Time-to-live for the request (value
   * we use).
   */
  int32_t ttl;

  /**
   * Number to mingle hashes for bloom-filter
   * tests with.
   */
  int32_t mingle;

  /**
   * Number of results that were found so far.
   */
  unsigned int results_found;
};


/**
 * Information we keep for each pending reply.  The
 * actual message follows at the end of this struct.
 */
struct PendingMessage
{
  /**
   * This is a linked list.
   */
  struct PendingMessage *next;

  /**
   * Size of the reply; actual reply message follows
   * at the end of this struct.
   */
  size_t msize;
  
  /**
   * How important is this message for us?
   */
  uint32_t priority;

};


/**
 * All requests from a client are kept in a doubly-linked list.
 */
struct ClientRequestList;


/**
 * Information we keep for each pending request.  We should try to
 * keep this struct as small as possible since its memory consumption
 * is key to how many requests we can have pending at once.
 */
struct PendingRequest
{

  /**
   * ID of a client making a request, NULL if this entry is for a
   * peer.
   */
  struct GNUNET_SERVER_Client *client;

  /**
   * If this request was made by a client,
   * this is our entry in the client request
   * list; otherwise NULL.
   */
  struct ClientRequestList *crl_entry;

  /**
   * If this is a namespace query, pointer to the hash of the public
   * key of the namespace; otherwise NULL.
   */
  GNUNET_HashCode *namespace;

  /**
   * Bloomfilter we use to filter out replies that we don't care about
   * (anymore).  NULL as long as we are interested in all replies.
   */
  struct GNUNET_CONTAINER_BloomFilter *bf;

  /**
   * Context of our GNUNET_CORE_peer_change_preference call.
   */
  struct GNUNET_CORE_InformationRequestContext *irc;

  /**
   * Handle for an active request for transmission to this peer, or
   * NULL.  Only used for replies that we are trying to send to a peer
   * that we are not yet connected to.
   */
  struct GNUNET_CORE_TransmitHandle *cth;

  /**
   * Replies that we have received but were unable to forward yet
   * (typically non-null only if we have a pending transmission
   * request with the client or the respective peer).
   */
  struct PendingMessage *replies_pending;

  /**
   * Pending transmission request for the target client (for processing of
   * 'replies_pending').
   */
  struct GNUNET_CONNECTION_TransmitHandle *th;

  /**
   * Hash code of all replies that we have seen so far (only valid
   * if client is not NULL since we only track replies like this for
   * our own clients).
   */
  GNUNET_HashCode *replies_seen;

  /**
   * Node in the heap representing this entry.
   */
  struct GNUNET_CONTAINER_HeapNode *hnode;

  /**
   * When did we first see this request (form this peer), or, if our
   * client is initiating, when did we last initiate a search?
   */
  struct GNUNET_TIME_Absolute start_time;

  /**
   * The query that this request is for.
   */
  GNUNET_HashCode query;

  /**
   * The task responsible for transmitting queries
   * for this request.
   */
  GNUNET_SCHEDULER_TaskIdentifier task;

  /**
   * (Interned) Peer identifier (only valid if "client" is NULL)
   * that identifies a peer that gave us this request.
   */
  GNUNET_PEER_Id source_pid;

  /**
   * (Interned) Peer identifier that identifies a preferred target
   * for requests.
   */
  GNUNET_PEER_Id target_pid;

  /**
   * (Interned) Peer identifiers of peers that have already
   * received our query for this content.
   */
  GNUNET_PEER_Id *used_pids;

  /**
   * Size of the 'bf' (in bytes).
   */
  size_t bf_size;

  /**
   * Desired anonymity level; only valid for requests from a local client.
   */
  uint32_t anonymity_level;

  /**
   * How many entries in "used_pids" are actually valid?
   */
  unsigned int used_pids_off;

  /**
   * How long is the "used_pids" array?
   */
  unsigned int used_pids_size;

  /**
   * How many entries in "replies_seen" are actually valid?
   */
  unsigned int replies_seen_off;

  /**
   * How long is the "replies_seen" array?
   */
  unsigned int replies_seen_size;
  
  /**
   * Priority with which this request was made.  If one of our clients
   * made the request, then this is the current priority that we are
   * using when initiating the request.  This value is used when
   * we decide to reward other peers with trust for providing a reply.
   */
  uint32_t priority;

  /**
   * Priority points left for us to spend when forwarding this request
   * to other peers.
   */
  uint32_t remaining_priority;

  /**
   * Number to mingle hashes for bloom-filter
   * tests with.
   */
  int32_t mingle;

  /**
   * TTL with which we saw this request (or, if we initiated, TTL that
   * we used for the request).
   */
  int32_t ttl;
  
  /**
   * Type of the content that this request is for.
   */
  uint32_t type;

};


/**
 * All requests from a client are kept in a doubly-linked list.
 */
struct ClientRequestList
{
  /**
   * This is a doubly-linked list.
   */
  struct ClientRequestList *next;

  /**
   * This is a doubly-linked list.
   */ 
  struct ClientRequestList *prev;

  /**
   * A request from this client.
   */
  struct PendingRequest *req;

  /**
   * Client list with the head and tail of this DLL.
   */
  struct ClientList *cl;
};


/**
 * Linked list of all clients that we are currently processing
 * requests for.
 */
struct ClientList
{

  /**
   * This is a linked list.
   */
  struct ClientList *next;

  /**
   * What client is this entry for?
   */
  struct GNUNET_SERVER_Client* client;

  /**
   * Head of the DLL of requests from this client.
   */
  struct ClientRequestList *head;

  /**
   * Tail of the DLL of requests from this client.
   */
  struct ClientRequestList *tail;

};


/**
 * Closure for "process_reply" function.
 */
struct ProcessReplyClosure
{
  /**
   * The data for the reply.
   */
  const void *data;

  /**
   * When the reply expires.
   */
  struct GNUNET_TIME_Absolute expiration;

  /**
   * Size of data.
   */
  size_t size;

  /**
   * Namespace that this reply belongs to
   * (if it is of type SBLOCK).
   */
  GNUNET_HashCode namespace;

  /**
   * Type of the block.
   */
  uint32_t type;

  /**
   * How much was this reply worth to us?
   */
  uint32_t priority;
};


/**
 * Information about a peer that we are connected to.
 * We track data that is useful for determining which
 * peers should receive our requests.
 */
struct ConnectedPeer
{

  /**
   * List of the last clients for which this peer
   * successfully answered a query. 
   */
  struct GNUNET_SERVER_Client *last_client_replies[CS2P_SUCCESS_LIST_SIZE];

  /**
   * List of the last PIDs for which
   * this peer successfully answered a query;
   * We use 0 to indicate no successful reply.
   */
  GNUNET_PEER_Id last_p2p_replies[P2P_SUCCESS_LIST_SIZE];

  /**
   * Average delay between sending the peer a request and
   * getting a reply (only calculated over the requests for
   * which we actually got a reply).   Calculated
   * as a moving average: new_delay = ((n-1)*last_delay+curr_delay) / n
   */ 
  struct GNUNET_TIME_Relative avg_delay;

  /**
   * Handle for an active request for transmission to this
   * peer, or NULL.
   */
  struct GNUNET_CORE_TransmitHandle *cth;

  /**
   * Messages (replies, queries, content migration) we would like to
   * send to this peer in the near future.  Sorted by priority.
   */
  struct PendingMessage *pending_messages;

  /**
   * Average priority of successful replies.  Calculated
   * as a moving average: new_avg = ((n-1)*last_avg+curr_prio) / n
   */
  double avg_priority;

  /**
   * The peer's identity.
   */
  GNUNET_PEER_Id pid;  

  /**
   * Number of requests we have currently pending with this peer (that
   * is, requests that were transmitted so recently that we would not
   * retransmit them right now).
   */
  unsigned int pending_requests;

  /**
   * Which offset in "last_p2p_replies" will be updated next?
   * (we go round-robin).
   */
  unsigned int last_p2p_replies_woff;

  /**
   * Which offset in "last_client_replies" will be updated next?
   * (we go round-robin).
   */
  unsigned int last_client_replies_woff;

};


/**
 * Our connection to the datastore.
 */
static struct GNUNET_DATASTORE_Handle *dsh;

/**
 * Our scheduler.
 */
static struct GNUNET_SCHEDULER_Handle *sched;

/**
 * Our configuration.
 */
const struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Handle to the core service (NULL until we've connected to it).
 */
struct GNUNET_CORE_Handle *core;

/**
 * Head of doubly-linked LGC list.
 */
static struct LocalGetContext *lgc_head;

/**
 * Tail of doubly-linked LGC list.
 */
static struct LocalGetContext *lgc_tail;

/**
 * Head of request queue for the datastore, sorted by timeout.
 */
static struct DatastoreRequestQueue *drq_head;

/**
 * Tail of request queue for the datastore.
 */
static struct DatastoreRequestQueue *drq_tail;

/**
 * Map of query hash codes to requests.
 */
static struct GNUNET_CONTAINER_MultiHashMap *requests_by_query;

/**
 * Map of peer IDs to requests (for those requests coming
 * from other peers).
 */
static struct GNUNET_CONTAINER_MultiHashMap *requests_by_peer;

/**
 * Linked list of all of our clients and their requests.
 */
static struct ClientList *clients;

/**
 * Heap with the request that will expire next at the top.  Contains
 * pointers of type "struct PendingRequest*"; these will *also* be
 * aliased from the "requests_by_peer" data structures and the
 * "requests_by_query" table.  Note that requests from our clients
 * don't expire and are thus NOT in the "requests_by_expiration"
 * (or the "requests_by_peer" tables).
 */
static struct GNUNET_CONTAINER_Heap *requests_by_expiration;

/**
 * Map of peer identifiers to "struct ConnectedPeer" (for that peer).
 */
static struct GNUNET_CONTAINER_MultiHashMap *connected_peers;

/**
 * Maximum number of requests (from other peers) that we're
 * willing to have pending at any given point in time.
 * FIXME: set from configuration (and 32 is a tiny value for testing only).
 */
static uint64_t max_pending_requests = 32;



/**
 * Run the next DS request in our
 * queue, we're done with the current one.
 */
static void
next_ds_request ()
{
  struct DatastoreRequestQueue *e;
  
  while (NULL != (e = drq_head))
    {
      if (0 != GNUNET_TIME_absolute_get_remaining (e->timeout).value)
	break;
      if (e->task != GNUNET_SCHEDULER_NO_TASK)
	GNUNET_SCHEDULER_cancel (sched, e->task);
      GNUNET_CONTAINER_DLL_remove (drq_head, drq_tail, e);
      e->req (e->req_cls, GNUNET_NO);
      GNUNET_free (e);  
    }
  if (e == NULL)
    return;
  if (e->task != GNUNET_SCHEDULER_NO_TASK)
    GNUNET_SCHEDULER_cancel (sched, e->task);
  e->task = GNUNET_SCHEDULER_NO_TASK;
  e->req (e->req_cls, GNUNET_YES);
  GNUNET_CONTAINER_DLL_remove (drq_head, drq_tail, e);
  GNUNET_free (e);  
}


/**
 * A datastore request had to be timed out. 
 *
 * @param cls closure (of type "struct DatastoreRequestQueue*")
 * @param tc task context, unused
 */
static void
timeout_ds_request (void *cls,
		    const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct DatastoreRequestQueue *e = cls;

  e->task = GNUNET_SCHEDULER_NO_TASK;
  GNUNET_CONTAINER_DLL_remove (drq_head, drq_tail, e);
  e->req (e->req_cls, GNUNET_NO);
  GNUNET_free (e);  
}


/**
 * Queue a request for the datastore.
 *
 * @param deadline by when the request should run
 * @param fun function to call once the request can be run
 * @param fun_cls closure for fun
 */
static struct DatastoreRequestQueue *
queue_ds_request (struct GNUNET_TIME_Relative deadline,
		  RequestFunction fun,
		  void *fun_cls)
{
  struct DatastoreRequestQueue *e;
  struct DatastoreRequestQueue *bef;

  if (drq_head == NULL)
    {
      /* no other requests pending, run immediately */
      fun (fun_cls, GNUNET_OK);
      return NULL;
    }
  e = GNUNET_malloc (sizeof (struct DatastoreRequestQueue));
  e->timeout = GNUNET_TIME_relative_to_absolute (deadline);
  e->req = fun;
  e->req_cls = fun_cls;
  if (deadline.value == GNUNET_TIME_UNIT_FOREVER_REL.value)
    {
      /* local request, highest prio, put at head of queue
	 regardless of deadline */
      bef = NULL;
    }
  else
    {
      bef = drq_tail;
      while ( (NULL != bef) &&
	      (e->timeout.value < bef->timeout.value) )
	bef = bef->prev;
    }
  GNUNET_CONTAINER_DLL_insert_after (drq_head, drq_tail, bef, e);
  if (deadline.value == GNUNET_TIME_UNIT_FOREVER_REL.value)
    return e;
  e->task = GNUNET_SCHEDULER_add_delayed (sched,
					  deadline,
					  &timeout_ds_request,
					  e);
  return e;				       
}


/**
 * Free the state associated with a local get context.
 *
 * @param lgc the lgc to free
 */
static void
local_get_context_free (struct LocalGetContext *lgc) 
{
  GNUNET_CONTAINER_DLL_remove (lgc_head, lgc_tail, lgc);
  GNUNET_SERVER_client_drop (lgc->client); 
  GNUNET_free_non_null (lgc->results);
  if (lgc->results_bf != NULL)
    GNUNET_CONTAINER_bloomfilter_free (lgc->results_bf);
  if (lgc->req != NULL)
    {
      if (lgc->req->task != GNUNET_SCHEDULER_NO_TASK)
	GNUNET_SCHEDULER_cancel (sched, lgc->req->task);
      GNUNET_CONTAINER_DLL_remove (lgc_head, lgc_tail, lgc);
      GNUNET_free (lgc->req);
    }
  GNUNET_free (lgc);
}


/**
 * We're able to transmit the next (local) result to the client.
 * Do it and ask the datastore for more.  Or, on error, tell
 * the datastore to stop giving us more.
 *
 * @param cls our closure (struct LocalGetContext)
 * @param max maximum number of bytes we can transmit
 * @param buf where to copy our message
 * @return number of bytes copied to buf
 */
static size_t
transmit_local_result (void *cls,
		       size_t max,
		       void *buf)
{
  struct LocalGetContext *lgc = cls;  
  uint16_t msize;

  if (NULL == buf)
    {
#if DEBUG_FS
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Failed to transmit result to local client, aborting datastore iteration.\n");
#endif
      /* error, abort! */
      GNUNET_free (lgc->result);
      lgc->result = NULL;
      GNUNET_DATASTORE_get_next (dsh, GNUNET_NO);
      return 0;
    }
  msize = ntohs (lgc->result->header.size);
#if DEBUG_FS
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
	      "Transmitting %u bytes of result to local client.\n",
	      msize);
#endif
  GNUNET_assert (max >= msize);
  memcpy (buf, lgc->result, msize);
  GNUNET_free (lgc->result);
  lgc->result = NULL;
  GNUNET_DATASTORE_get_next (dsh, GNUNET_YES);
  return msize;
}


/**
 * Mingle hash with the mingle_number to produce different bits.
 */
static void
mingle_hash (const GNUNET_HashCode * in,
	     int32_t mingle_number, 
	     GNUNET_HashCode * hc)
{
  GNUNET_HashCode m;

  GNUNET_CRYPTO_hash (&mingle_number, 
		      sizeof (int32_t), 
		      &m);
  GNUNET_CRYPTO_hash_xor (&m, in, hc);
}


/**
 * How many bytes should a bloomfilter be if we have already seen
 * entry_count responses?  Note that BLOOMFILTER_K gives us the number
 * of bits set per entry.  Furthermore, we should not re-size the
 * filter too often (to keep it cheap).
 *
 * Since other peers will also add entries but not resize the filter,
 * we should generally pick a slightly larger size than what the
 * strict math would suggest.
 *
 * @return must be a power of two and smaller or equal to 2^15.
 */
static size_t
compute_bloomfilter_size (unsigned int entry_count)
{
  size_t size;
  unsigned int ideal = (entry_count * BLOOMFILTER_K) / 4;
  uint16_t max = 1 << 15;

  if (entry_count > max)
    return max;
  size = 8;
  while ((size < max) && (size < ideal))
    size *= 2;
  if (size > max)
    return max;
  return size;
}


/**
 * Recalculate our bloom filter for filtering replies.
 *
 * @param count number of entries we are filtering right now
 * @param mingle set to our new mingling value
 * @param bf_size set to the size of the bloomfilter
 * @param entries the entries to filter
 * @return updated bloomfilter, NULL for none
 */
static struct GNUNET_CONTAINER_BloomFilter *
refresh_bloomfilter (unsigned int count,
		     int32_t * mingle,
		     size_t *bf_size,
		     const GNUNET_HashCode *entries)
{
  struct GNUNET_CONTAINER_BloomFilter *bf;
  size_t nsize;
  unsigned int i;
  GNUNET_HashCode mhash;

  if (0 == count)
    return NULL;
  nsize = compute_bloomfilter_size (count);
  *mingle = GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK, -1);
  *bf_size = nsize;
  bf = GNUNET_CONTAINER_bloomfilter_init (NULL, 
					  nsize,
					  BLOOMFILTER_K);
  for (i=0;i<count;i++)
    {
      mingle_hash (&entries[i], *mingle, &mhash);
      GNUNET_CONTAINER_bloomfilter_add (bf, &mhash);
    }
  return bf;
}


/**
 * Closure used for "target_peer_select_cb".
 */
struct PeerSelectionContext 
{
  /**
   * The request for which we are selecting
   * peers.
   */
  struct PendingRequest *pr;

  /**
   * Current "prime" target.
   */
  struct GNUNET_PeerIdentity target;

  /**
   * How much do we like this target?
   */
  double target_score;

};


/**
 * Function called for each connected peer to determine
 * which one(s) would make good targets for forwarding.
 *
 * @param cls closure (struct PeerSelectionContext)
 * @param key current key code (peer identity)
 * @param value value in the hash map (struct ConnectedPeer)
 * @return GNUNET_YES if we should continue to
 *         iterate,
 *         GNUNET_NO if not.
 */
static int
target_peer_select_cb (void *cls,
		       const GNUNET_HashCode * key,
		       void *value)
{
  struct PeerSelectionContext *psc = cls;
  struct ConnectedPeer *cp = value;
  struct PendingRequest *pr = psc->pr;
  double score;
  unsigned int i;

  /* 1) check if we have already (recently) forwarded to this peer */
  for (i=0;i<pr->used_pids_off;i++)
    if (pr->used_pids[i] == cp->pid)
      return GNUNET_YES; /* skip */
  // 2) calculate how much we'd like to forward to this peer
  score = 0; // FIXME!
  
  /* store best-fit in closure */
  if (score > psc->target_score)
    {
      psc->target_score = score;
      psc->target.hashPubKey = *key; 
    }
  return GNUNET_YES;
}


/**
 * We use a random delay to make the timing of requests
 * less predictable.  This function returns such a random
 * delay.
 *
 * @return random delay to use for some request, between 0 and TTL_DECREMENT ms
 */
static struct GNUNET_TIME_Relative
get_processing_delay ()
{
  return GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_MILLISECONDS,
					GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK,
								  TTL_DECREMENT));
}


/**
 * Task that is run for each request with the goal of forwarding the
 * associated query to other peers.  The task should re-schedule
 * itself to be re-run once the TTL has expired.  (or at a later time
 * if more peers should be queried earlier).
 *
 * @param cls the requests "struct PendingRequest*"
 * @param tc task context (unused)
 */
static void
forward_request_task (void *cls,
		      const struct GNUNET_SCHEDULER_TaskContext *tc);


/**
 * We've selected a peer for forwarding of a query.  Construct the
 * message and then re-schedule the task to forward again to (other)
 * peers.
 *
 * @param cls closure
 * @param size number of bytes available in buf
 * @param buf where the callee should write the message
 * @return number of bytes written to buf
 */
static size_t
transmit_request_cb (void *cls,
		     size_t size, 
		     void *buf)
{
  struct ConnectedPeer *cp = cls;
  char *cbuf = buf;
  struct GNUNET_PeerIdentity target;
  struct PendingMessage *pr;
  size_t tot;

  cp->cth = NULL;
  tot = 0;
  while ( (NULL != (pr = cp->pending_messages)) &&
	  (pr->msize < size - tot) )
    {
      memcpy (&cbuf[tot],
	      &pr[1],
	      pr->msize);
      tot += pr->msize;
      cp->pending_messages = pr->next;
      GNUNET_free (pr);
    }
  if (NULL != pr)
    {
      GNUNET_PEER_resolve (cp->pid,
			   &target);    
      cp->cth = GNUNET_CORE_notify_transmit_ready (core,
						   pr->priority,
						   GNUNET_TIME_UNIT_FOREVER_REL,
						   &target,
						   pr->msize,
						   &transmit_request_cb,
						   cp);
    }
  return tot;
}


/**
 * Function called after we've tried to reserve a certain amount of
 * bandwidth for a reply.  Check if we succeeded and if so send our
 * query.
 *
 * @param cls the requests "struct PendingRequest*"
 * @param peer identifies the peer
 * @param bpm_in set to the current bandwidth limit (receiving) for this peer
 * @param bpm_out set to the current bandwidth limit (sending) for this peer
 * @param amount set to the amount that was actually reserved or unreserved
 * @param preference current traffic preference for the given peer
 */
static void
target_reservation_cb (void *cls,
		       const struct
		       GNUNET_PeerIdentity * peer,
		       unsigned int bpm_in,
		       unsigned int bpm_out,
		       int amount,
		       uint64_t preference)
{
  struct PendingRequest *pr = cls;
  struct ConnectedPeer *cp;
  struct PendingMessage *pm;
  struct PendingMessage *pos;
  struct PendingMessage *prev;
  struct GetMessage *gm;
  GNUNET_HashCode *ext;
  char *bfdata;
  size_t msize;
  unsigned int k;

  pr->task = GNUNET_SCHEDULER_add_delayed (sched,
					   get_processing_delay (), // FIXME: longer?
					   &forward_request_task,
					   pr);
  pr->irc = NULL;
  GNUNET_assert (peer != NULL);
  if (amount != DBLOCK_SIZE) 
    {
      /* FIXME: call stats... */
      return; /* this target round failed */    
    }
  // (2) transmit, update ttl/priority
  cp = GNUNET_CONTAINER_multihashmap_get (connected_peers,
					  &peer->hashPubKey);
  if (cp == NULL)
    {
      /* Peer must have just left; try again immediately */
      pr->task = GNUNET_SCHEDULER_add_now (sched,
					   &forward_request_task,
					   pr);
      return;
    }
  /* build message and insert message into priority queue */
  k = 0; // FIXME: count hash codes!
  msize = sizeof (struct GetMessage) + pr->bf_size + k * sizeof(GNUNET_HashCode);
  GNUNET_assert (msize < GNUNET_SERVER_MAX_MESSAGE_SIZE);
  pm = GNUNET_malloc (sizeof (struct PendingMessage) + msize);
  pm->msize = msize;
  pm->priority = 0; // FIXME: calculate priority properly!
  gm = (struct GetMessage*) &pm[1];
  gm->header.type = htons (GNUNET_MESSAGE_TYPE_FS_GET);
  gm->header.size = htons (msize);
  gm->type = htonl (pr->type);
  pr->remaining_priority /= 2;
  gm->priority = htonl (pr->remaining_priority);
  gm->ttl = htonl (pr->ttl);
  gm->filter_mutator = htonl(pr->mingle);
  gm->hash_bitmap = htonl (42);
  gm->query = pr->query;
  ext = (GNUNET_HashCode*) &gm[1];

  // FIXME: setup "ext[0]..[k-1]"
  bfdata = (char *) &ext[k];
  if (pr->bf != NULL)
    GNUNET_CONTAINER_bloomfilter_get_raw_data (pr->bf,
					       bfdata,
					       pr->bf_size);


  prev = NULL;
  pos = cp->pending_messages;
  while ( (pos != NULL) &&
	  (pm->priority < pos->priority) )
    {
      prev = pos;
      pos = pos->next;
    }
  if (prev == NULL)
    cp->pending_messages = pm;
  else
    prev->next = pm;
  pm->next = pos;
  if (cp->cth == NULL)
    cp->cth = GNUNET_CORE_notify_transmit_ready (core,
						 cp->pending_messages->priority,
						 GNUNET_TIME_UNIT_FOREVER_REL,
						 peer,
						 msize,
						 &transmit_request_cb,
						 cp);
  if (cp->cth == NULL)
    {
      /* technically, this should not be a 'break'; but
	 we don't handle this (rare) case yet, so let's warn
	 about it... */
      GNUNET_break (0);
      // FIXME: now what?
    }
}


/**
 * Task that is run for each request with the goal of forwarding the
 * associated query to other peers.  The task should re-schedule
 * itself to be re-run once the TTL has expired.  (or at a later time
 * if more peers should be queried earlier).
 *
 * @param cls the requests "struct PendingRequest*"
 * @param tc task context (unused)
 */
static void
forward_request_task (void *cls,
		      const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct PendingRequest *pr = cls;
  struct PeerSelectionContext psc;

  pr->task = GNUNET_SCHEDULER_NO_TASK;
  /* (1) select target */
  psc.pr = pr;
  psc.target_score = DBL_MIN;
  GNUNET_CONTAINER_multihashmap_iterate (connected_peers,
					 &target_peer_select_cb,
					 &psc);
  if (psc.target_score == DBL_MIN)
    {
      /* no possible target found, wait some time */
      pr->task = GNUNET_SCHEDULER_add_delayed (sched,
					       get_processing_delay (), // FIXME: exponential back-off? or at least wait longer...
					       &forward_request_task,
					       pr);
      return;
    }
  /* (2) reserve reply bandwidth */
  GNUNET_assert (NULL == pr->irc);
  pr->irc = GNUNET_CORE_peer_change_preference (sched, cfg,
						&psc.target,
						GNUNET_CONSTANTS_SERVICE_TIMEOUT, 
						-1,
						DBLOCK_SIZE, // FIXME: make dependent on type?
						0,
						&target_reservation_cb,
						pr);
}


/**
 * We're processing (local) results for a search request
 * from a (local) client.  Pass applicable results to the
 * client and if we are done either clean up (operation
 * complete) or switch to P2P search (more results possible).
 *
 * @param cls our closure (struct LocalGetContext)
 * @param key key for the content
 * @param size number of bytes in data
 * @param data content stored
 * @param type type of the content
 * @param priority priority of the content
 * @param anonymity anonymity-level for the content
 * @param expiration expiration time for the content
 * @param uid unique identifier for the datum;
 *        maybe 0 if no unique identifier is available
 */
static void
process_local_get_result (void *cls,
			  const GNUNET_HashCode * key,
			  uint32_t size,
			  const void *data,
			  uint32_t type,
			  uint32_t priority,
			  uint32_t anonymity,
			  struct GNUNET_TIME_Absolute
			  expiration, 
			  uint64_t uid)
{
  struct LocalGetContext *lgc = cls;
  struct PendingRequest *pr;
  struct ClientRequestList *crl;
  struct ClientList *cl;
  size_t msize;
  unsigned int i;

  if (key == NULL)
    {
#if DEBUG_FS
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Received last result for `%s' from local datastore, deciding what to do next.\n",
		  GNUNET_h2s (&lgc->query));
#endif
      /* no further results from datastore; continue
	 processing further requests from the client and
	 allow the next task to use the datastore; also,
	 switch to P2P requests or clean up our state. */
      next_ds_request ();
      GNUNET_SERVER_receive_done (lgc->client,
				  GNUNET_OK);
      if ( (lgc->results_used == 0) ||
	   (lgc->type == GNUNET_DATASTORE_BLOCKTYPE_KBLOCK) ||
	   (lgc->type == GNUNET_DATASTORE_BLOCKTYPE_SBLOCK) ||
	   (lgc->type == GNUNET_DATASTORE_BLOCKTYPE_SKBLOCK) )
	{
#if DEBUG_FS
	  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		      "Forwarding query for `%s' to network.\n",
		      GNUNET_h2s (&lgc->query));
#endif
	  cl = clients;
	  while ( (NULL != cl) &&
		  (cl->client != lgc->client) )
	    cl = cl->next;
	  if (cl == NULL)
	    {
	      cl = GNUNET_malloc (sizeof (struct ClientList));
	      cl->client = lgc->client;
	      cl->next = clients;
	      clients = cl;
	    }
	  crl = GNUNET_malloc (sizeof (struct ClientRequestList));
	  crl->cl = cl;
	  GNUNET_CONTAINER_DLL_insert (cl->head, cl->tail, crl);
	  pr = GNUNET_malloc (sizeof (struct PendingRequest));
	  pr->client = lgc->client;
	  GNUNET_SERVER_client_keep (pr->client);
	  pr->crl_entry = crl;
	  crl->req = pr;
	  if (lgc->type == GNUNET_DATASTORE_BLOCKTYPE_SBLOCK)
	    {
	      pr->namespace = GNUNET_malloc (sizeof (GNUNET_HashCode));
	      *pr->namespace = lgc->namespace;
	    }
	  pr->replies_seen = lgc->results;
	  lgc->results = NULL;
	  pr->start_time = GNUNET_TIME_absolute_get ();
	  pr->query = lgc->query;
	  pr->target_pid = GNUNET_PEER_intern (&lgc->target);
	  pr->replies_seen_off = lgc->results_used;
	  pr->replies_seen_size = lgc->results_size;
	  lgc->results_size = 0;
	  pr->type = lgc->type;
	  pr->anonymity_level = lgc->anonymity_level;
	  pr->bf = refresh_bloomfilter (pr->replies_seen_off,
					&pr->mingle,
					&pr->bf_size,
					pr->replies_seen);
	  GNUNET_CONTAINER_multihashmap_put (requests_by_query,
					     &pr->query,
					     pr,
					     GNUNET_CONTAINER_MULTIHASHMAPOPTION_MULTIPLE);
	  pr->task = GNUNET_SCHEDULER_add_delayed (sched,
						   get_processing_delay (),
						   &forward_request_task,
						   pr);
	  local_get_context_free (lgc);
   	  return;
	}
      /* got all possible results, clean up! */
#if DEBUG_FS
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Found all possible results for query for `%s', done!\n",
		  GNUNET_h2s (&lgc->query));
#endif
      local_get_context_free (lgc);
      return;
    }
  if (type == GNUNET_DATASTORE_BLOCKTYPE_ONDEMAND)
    {
#if DEBUG_FS
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Received on-demand block for `%s' from local datastore, fetching data.\n",
		  GNUNET_h2s (&lgc->query));
#endif
      GNUNET_FS_handle_on_demand_block (key, size, data, type, priority, 
					anonymity, expiration, uid,
					dsh,
					&process_local_get_result,
					lgc);
      return;
    }
  if ( (type != lgc->type) &&
       (lgc->type != GNUNET_DATASTORE_BLOCKTYPE_ANY) )
    {
      /* this should be virtually impossible to reach (DBLOCK 
	 query hash being identical to KBLOCK/SBLOCK query hash);
	 nevertheless, if it happens, the correct thing is to
	 simply skip the result. */
#if DEBUG_FS
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Received block of unexpected type (%u, want %u) for `%s' from local datastore, ignoring.\n",
		  type,
		  lgc->type,
		  GNUNET_h2s (&lgc->query));
#endif
      GNUNET_DATASTORE_get_next (dsh, GNUNET_YES);	  
      return;
    }
  /* check if this is a result we've alredy
     received */
  for (i=0;i<lgc->results_used;i++)
    if (0 == memcmp (key,
		     &lgc->results[i],
		     sizeof (GNUNET_HashCode)))
      {
#if DEBUG_FS
	GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		    "Received duplicate result for `%s' from local datastore, ignoring.\n",
		    GNUNET_h2s (&lgc->query));
#endif
	GNUNET_DATASTORE_get_next (dsh, GNUNET_YES);
	return;	
      }
  if (lgc->results_used == lgc->results_size)
    GNUNET_array_grow (lgc->results,
		       lgc->results_size,
		       lgc->results_size * 2 + 2);
  GNUNET_CRYPTO_hash (data, 
		      size, 
		      &lgc->results[lgc->results_used++]);    
  msize = size + sizeof (struct ContentMessage);
  GNUNET_assert (msize < GNUNET_SERVER_MAX_MESSAGE_SIZE);
  lgc->result = GNUNET_malloc (msize);
  lgc->result->header.size = htons (msize);
  lgc->result->header.type = htons (GNUNET_MESSAGE_TYPE_FS_CONTENT);
  lgc->result->type = htonl (type);
  lgc->result->expiration = GNUNET_TIME_absolute_hton (expiration);
  memcpy (&lgc->result[1],
	  data,
	  size);
#if DEBUG_FS
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
	      "Received new result for `%s' from local datastore, passing to client.\n",
	      GNUNET_h2s (&lgc->query));
#endif
  GNUNET_SERVER_notify_transmit_ready (lgc->client,
				       msize,
				       GNUNET_TIME_UNIT_FOREVER_REL,
				       &transmit_local_result,
				       lgc);
}


/**
 * We're processing a search request from a local
 * client.  Now it is our turn to query the datastore.
 * 
 * @param cls our closure (struct LocalGetContext)
 * @param tc unused
 */
static void
transmit_local_get (void *cls,
		    const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct LocalGetContext *lgc = cls;
  uint32_t type;
  
  type = lgc->type;
  if (type == GNUNET_DATASTORE_BLOCKTYPE_DBLOCK)
    type = GNUNET_DATASTORE_BLOCKTYPE_ANY; /* to get on-demand as well */
  GNUNET_DATASTORE_get (dsh,
			&lgc->query,
			type,
			&process_local_get_result,
			lgc,
			GNUNET_TIME_UNIT_FOREVER_REL);
}


/**
 * We're processing a search request from a local
 * client.  Now it is our turn to query the datastore.
 * 
 * @param cls our closure (struct LocalGetContext)
 * @param ok did we succeed to queue for datastore access, should always be GNUNET_OK
 */
static void 
transmit_local_get_ready (void *cls,
			  int ok)
{
  struct LocalGetContext *lgc = cls;

  GNUNET_assert (GNUNET_OK == ok);
  GNUNET_SCHEDULER_add_continuation (sched,
				     &transmit_local_get,
				     lgc,
				     GNUNET_SCHEDULER_REASON_PREREQ_DONE);
}


/**
 * Handle START_SEARCH-message (search request from client).
 *
 * @param cls closure
 * @param client identification of the client
 * @param message the actual message
 */
static void
handle_start_search (void *cls,
		     struct GNUNET_SERVER_Client *client,
		     const struct GNUNET_MessageHeader *message)
{
  const struct SearchMessage *sm;
  struct LocalGetContext *lgc;
  uint16_t msize;
  unsigned int sc;
  
  msize = ntohs (message->size);
  if ( (msize < sizeof (struct SearchMessage)) ||
       (0 != (msize - sizeof (struct SearchMessage)) % sizeof (GNUNET_HashCode)) )
    {
      GNUNET_break (0);
      GNUNET_SERVER_receive_done (client,
				  GNUNET_SYSERR);
      return;
    }
  sc = (msize - sizeof (struct SearchMessage)) / sizeof (GNUNET_HashCode);
  sm = (const struct SearchMessage*) message;
  GNUNET_SERVER_client_keep (client);
  lgc = GNUNET_malloc (sizeof (struct LocalGetContext));
  if  (sc > 0)
    {
      lgc->results_used = sc;
      GNUNET_array_grow (lgc->results,
			 lgc->results_size,
			 sc * 2);
      memcpy (lgc->results,
	      &sm[1],
	      sc * sizeof (GNUNET_HashCode));
    }
  lgc->client = client;
  lgc->type = ntohl (sm->type);
  lgc->anonymity_level = ntohl (sm->anonymity_level);
  switch (lgc->type)
    {
    case GNUNET_DATASTORE_BLOCKTYPE_DBLOCK:
    case GNUNET_DATASTORE_BLOCKTYPE_IBLOCK:
      lgc->target.hashPubKey = sm->target;
      break;
    case GNUNET_DATASTORE_BLOCKTYPE_SBLOCK:
      lgc->namespace = sm->target;
      break;
    default:
      break;
    }
  lgc->query = sm->query;
  GNUNET_CONTAINER_DLL_insert (lgc_head, lgc_tail, lgc);
  lgc->req = queue_ds_request (GNUNET_TIME_UNIT_FOREVER_REL,
			       &transmit_local_get_ready,
			       lgc);
}


/**
 * List of handlers for the messages understood by this
 * service.
 */
static struct GNUNET_SERVER_MessageHandler handlers[] = {
  {&GNUNET_FS_handle_index_start, NULL, 
   GNUNET_MESSAGE_TYPE_FS_INDEX_START, 0},
  {&GNUNET_FS_handle_index_list_get, NULL, 
   GNUNET_MESSAGE_TYPE_FS_INDEX_LIST_GET, sizeof(struct GNUNET_MessageHeader) },
  {&GNUNET_FS_handle_unindex, NULL, GNUNET_MESSAGE_TYPE_FS_UNINDEX, 
   sizeof (struct UnindexMessage) },
  {&handle_start_search, NULL, GNUNET_MESSAGE_TYPE_FS_START_SEARCH, 
   0 },
  {NULL, NULL, 0, 0}
};


/**
 * Clean up the memory used by the PendingRequest structure (except
 * for the client or peer list that the request may be part of).
 *
 * @param pr request to clean up
 */
static void
destroy_pending_request (struct PendingRequest *pr)
{
  struct PendingMessage *reply;
  struct ClientList *cl;

  GNUNET_CONTAINER_multihashmap_remove (requests_by_query,
					&pr->query,
					pr);
  // FIXME: not sure how this can work (efficiently)
  // also, what does the return value mean?
  if (pr->irc != NULL)
    {
      GNUNET_CORE_peer_change_preference_cancel (pr->irc);
      pr->irc = NULL;
    }
  if (pr->client == NULL)
    {
      GNUNET_CONTAINER_heap_remove_node (requests_by_expiration,
					 pr->hnode);
    }
  else
    {
      cl = pr->crl_entry->cl;
      GNUNET_CONTAINER_DLL_remove (cl->head,
				   cl->tail,
				   pr->crl_entry);
    }
  if (GNUNET_SCHEDULER_NO_TASK != pr->task)
    GNUNET_SCHEDULER_cancel (sched, pr->task);
  if (NULL != pr->bf)
    GNUNET_CONTAINER_bloomfilter_free (pr->bf);
  if (NULL != pr->th)
    GNUNET_CONNECTION_notify_transmit_ready_cancel (pr->th);
  while (NULL != (reply = pr->replies_pending))
    {
      pr->replies_pending = reply->next;
      GNUNET_free (reply);
    }
  if (NULL != pr->cth)
    GNUNET_CORE_notify_transmit_ready_cancel (pr->cth);
  GNUNET_PEER_change_rc (pr->source_pid, -1);
  GNUNET_PEER_change_rc (pr->target_pid, -1);
  GNUNET_PEER_decrement_rcs (pr->used_pids, pr->used_pids_off);
  GNUNET_free_non_null (pr->used_pids);
  GNUNET_free_non_null (pr->replies_seen);
  GNUNET_free_non_null (pr->namespace);
  GNUNET_free (pr);
}


/**
 * A client disconnected.  Remove all of its pending queries.
 *
 * @param cls closure, NULL
 * @param client identification of the client
 */
static void
handle_client_disconnect (void *cls,
			  struct GNUNET_SERVER_Client
			  * client)
{
  struct LocalGetContext *lgc;
  struct ClientList *cpos;
  struct ClientList *cprev;
  struct ClientRequestList *rl;

  if (client == NULL)
    return;
  lgc = lgc_head;
  while ( (NULL != lgc) &&
	  (lgc->client != client) )
    lgc = lgc->next;
  if (lgc != NULL)
    local_get_context_free (lgc);
  cprev = NULL;
  cpos = clients;
  while ( (NULL != cpos) &&
	  (clients->client != client) )
    {
      cprev = cpos;
      cpos = cpos->next;
    }
  if (cpos != NULL)
    {
      if (cprev == NULL)
	clients = cpos->next;
      else
	cprev->next = cpos->next;
      while (NULL != (rl = cpos->head))
	{
	  cpos->head = rl->next;
	  destroy_pending_request (rl->req);
	  GNUNET_free (rl);
	}
      GNUNET_free (cpos);
    }
}


/**
 * Iterator over entries in the "requests_by_query" map
 * that frees all the entries.
 *
 * @param cls closure, NULL
 * @param key current key code (the query, unused) 
 * @param value value in the hash map, of type "struct PendingRequest*"
 * @return GNUNET_YES (we should continue to  iterate)
 */
static int 
destroy_pending_request_cb (void *cls,
			    const GNUNET_HashCode * key,
			    void *value)
{
  struct PendingRequest *pr = value;

  destroy_pending_request (pr);
  return GNUNET_YES;
}


/**
 * Task run during shutdown.
 *
 * @param cls unused
 * @param tc unused
 */
static void
shutdown_task (void *cls,
	       const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  if (NULL != core)
    {
      GNUNET_CORE_disconnect (core);
      core = NULL;
    }
  if (NULL != dsh)
    {
      GNUNET_DATASTORE_disconnect (dsh,
				   GNUNET_NO);
      dsh = NULL;
    }
  GNUNET_CONTAINER_multihashmap_iterate (requests_by_query,
					 &destroy_pending_request_cb,
					 NULL);
  while (clients != NULL)
    handle_client_disconnect (NULL,
			      clients->client);
  GNUNET_CONTAINER_multihashmap_destroy (requests_by_query);
  requests_by_query = NULL;
  GNUNET_CONTAINER_multihashmap_destroy (requests_by_peer);
  requests_by_peer = NULL;
  GNUNET_CONTAINER_heap_destroy (requests_by_expiration);
  requests_by_expiration = NULL;
  // FIXME: iterate over entries and free individually?
  // (or do we get disconnect notifications?)
  GNUNET_CONTAINER_multihashmap_destroy (connected_peers);
  connected_peers = NULL;
}


/**
 * Free (each) request made by the peer.
 *
 * @param cls closure, points to peer that the request belongs to
 * @param key current key code
 * @param value value in the hash map
 * @return GNUNET_YES (we should continue to iterate)
 */
static int
destroy_request (void *cls,
		 const GNUNET_HashCode * key,
		 void *value)
{
  const struct GNUNET_PeerIdentity * peer = cls;
  struct PendingRequest *pr = value;
  
  GNUNET_CONTAINER_multihashmap_remove (requests_by_peer,
					&peer->hashPubKey,
					pr);
  destroy_pending_request (pr);
  return GNUNET_YES;
}



/**
 * Method called whenever a given peer connects.
 *
 * @param cls closure, not used
 * @param peer peer identity this notification is about
 * @param latency reported latency of the connection with 'other'
 * @param distance reported distance (DV) to 'other' 
 */
static void 
peer_connect_handler (void *cls,
		      const struct
		      GNUNET_PeerIdentity * peer,
		      struct GNUNET_TIME_Relative latency,
		      uint32_t distance)
{
  struct ConnectedPeer *cp;

  cp = GNUNET_malloc (sizeof (struct ConnectedPeer));
  cp->pid = GNUNET_PEER_intern (peer);
  GNUNET_CONTAINER_multihashmap_put (connected_peers,
				     &peer->hashPubKey,
				     cp,
				     GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY);
}


/**
 * Method called whenever a peer disconnects.
 *
 * @param cls closure, not used
 * @param peer peer identity this notification is about
 */
static void
peer_disconnect_handler (void *cls,
			 const struct
			 GNUNET_PeerIdentity * peer)
{
  struct ConnectedPeer *cp;
  struct PendingMessage *pm;

  cp = GNUNET_CONTAINER_multihashmap_get (connected_peers,
					  &peer->hashPubKey);
  if (cp != NULL)
    {
      GNUNET_PEER_change_rc (cp->pid, -1);
      GNUNET_PEER_decrement_rcs (cp->last_p2p_replies, P2P_SUCCESS_LIST_SIZE);
      if (NULL != cp->cth)
	GNUNET_CORE_notify_transmit_ready_cancel (cp->cth);
      while (NULL != (pm = cp->pending_messages))
	{
	  cp->pending_messages = pm->next;
	  GNUNET_free (pm);
	}
      GNUNET_free (cp);
    }
  GNUNET_CONTAINER_multihashmap_get_multiple (requests_by_peer,
					      &peer->hashPubKey,
					      &destroy_request,
					      (void*) peer);
}


/**
 * We're processing a GET request from another peer and have decided
 * to forward it to other peers.
 *
 * @param cls our "struct ProcessGetContext *"
 * @param tc unused
 */
static void
forward_get_request (void *cls,
		     const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct ProcessGetContext *pgc = cls;
  struct PendingRequest *pr;
  struct PendingRequest *eer;
  struct GNUNET_PeerIdentity target;

  pr = GNUNET_malloc (sizeof (struct PendingRequest));
  if (GET_MESSAGE_BIT_SKS_NAMESPACE == (GET_MESSAGE_BIT_SKS_NAMESPACE & pgc->bm))
    {
      pr->namespace = GNUNET_malloc (sizeof(GNUNET_HashCode));
      *pr->namespace = pgc->namespace;
    }
  pr->bf = pgc->bf;
  pr->bf_size = pgc->bf_size;
  pgc->bf = NULL;
  pr->start_time = pgc->start_time;
  pr->query = pgc->query;
  pr->source_pid = GNUNET_PEER_intern (&pgc->reply_to);
  if (GET_MESSAGE_BIT_TRANSMIT_TO == (GET_MESSAGE_BIT_TRANSMIT_TO & pgc->bm))
    pr->target_pid = GNUNET_PEER_intern (&pgc->prime_target);
  pr->anonymity_level = 1; /* default */
  pr->priority = pgc->priority;
  pr->remaining_priority = pr->priority;
  pr->mingle = pgc->mingle;
  pr->ttl = pgc->ttl; 
  pr->type = pgc->type;
  GNUNET_CONTAINER_multihashmap_put (requests_by_query,
				     &pr->query,
				     pr,
				     GNUNET_CONTAINER_MULTIHASHMAPOPTION_MULTIPLE);
  GNUNET_CONTAINER_multihashmap_put (requests_by_peer,
				     &pgc->reply_to.hashPubKey,
				     pr,
				     GNUNET_CONTAINER_MULTIHASHMAPOPTION_MULTIPLE);
  pr->hnode = GNUNET_CONTAINER_heap_insert (requests_by_expiration,
					    pr,
					    pr->start_time.value + pr->ttl);
  if (GNUNET_CONTAINER_heap_get_size (requests_by_expiration) > max_pending_requests)
    {
      /* expire oldest request! */
      eer = GNUNET_CONTAINER_heap_peek (requests_by_expiration);
      GNUNET_PEER_resolve (eer->source_pid,
			   &target);    
      GNUNET_CONTAINER_multihashmap_remove (requests_by_peer,
					    &target.hashPubKey,
					    eer);
      destroy_pending_request (eer);     
    }
  pr->task = GNUNET_SCHEDULER_add_delayed (sched,
					   get_processing_delay (),
					   &forward_request_task,
					   pr);
  GNUNET_free (pgc); 
}
/**
 * Transmit the given message by copying it to
 * the target buffer "buf".  "buf" will be
 * NULL and "size" zero if the socket was closed for
 * writing in the meantime.  In that case, only

 * free the message
 *
 * @param cls closure, pointer to the message
 * @param size number of bytes available in buf
 * @param buf where the callee should write the message
 * @return number of bytes written to buf
 */
static size_t
transmit_message (void *cls,
		  size_t size, void *buf)
{
  struct GNUNET_MessageHeader *msg = cls;
  uint16_t msize;
  
  if (NULL == buf)
    {
#if DEBUG_FS
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Dropping reply, core too busy.\n");
#endif
      GNUNET_free (msg);
      return 0;
    }
  msize = ntohs (msg->size);
  GNUNET_assert (size >= msize);
  memcpy (buf, msg, msize);
  GNUNET_free (msg);
  return msize;
}


/**
 * Test if the load on this peer is too high
 * to even consider processing the query at
 * all.
 * 
 * @return GNUNET_YES if the load is too high, GNUNET_NO otherwise
 */
static int
test_load_too_high ()
{
  return GNUNET_NO; // FIXME
}


/**
 * We're processing (local) results for a search request
 * from another peer.  Pass applicable results to the
 * peer and if we are done either clean up (operation
 * complete) or forward to other peers (more results possible).
 *
 * @param cls our closure (struct LocalGetContext)
 * @param key key for the content
 * @param size number of bytes in data
 * @param data content stored
 * @param type type of the content
 * @param priority priority of the content
 * @param anonymity anonymity-level for the content
 * @param expiration expiration time for the content
 * @param uid unique identifier for the datum;
 *        maybe 0 if no unique identifier is available
 */
static void
process_p2p_get_result (void *cls,
			const GNUNET_HashCode * key,
			uint32_t size,
			const void *data,
			uint32_t type,
			uint32_t priority,
			uint32_t anonymity,
			struct GNUNET_TIME_Absolute
			expiration, 
			uint64_t uid)
{
  struct ProcessGetContext *pgc = cls;
  GNUNET_HashCode dhash;
  GNUNET_HashCode mhash;
  struct PutMessage *reply;
  
  if (NULL == key)
    {
      /* no more results */
      if ( ( (pgc->policy & ROUTING_POLICY_FORWARD) ==  ROUTING_POLICY_FORWARD) &&
	   ( (0 == pgc->results_found) ||
	     (pgc->type == GNUNET_DATASTORE_BLOCKTYPE_KBLOCK) ||
	     (pgc->type == GNUNET_DATASTORE_BLOCKTYPE_SBLOCK) ||
	     (pgc->type == GNUNET_DATASTORE_BLOCKTYPE_SKBLOCK) ) )
	{
	  GNUNET_SCHEDULER_add_continuation (sched,
					     &forward_get_request,
					     pgc,
					     GNUNET_SCHEDULER_REASON_PREREQ_DONE);
	}
      else
	{
	  if (pgc->bf != NULL)
	    GNUNET_CONTAINER_bloomfilter_free (pgc->bf);
	  GNUNET_free (pgc); 
	}
      next_ds_request ();
      return;
    }
  if (type == GNUNET_DATASTORE_BLOCKTYPE_ONDEMAND)
    {
      GNUNET_FS_handle_on_demand_block (key, size, data, type, priority, 
					anonymity, expiration, uid, dsh,
					&process_p2p_get_result,
					pgc);
      return;
    }
  /* check for duplicates */
  GNUNET_CRYPTO_hash (data, size, &dhash);
  mingle_hash (&dhash, 
	       pgc->mingle,
	       &mhash);
  if ( (pgc->bf != NULL) &&
       (GNUNET_YES ==
	GNUNET_CONTAINER_bloomfilter_test (pgc->bf,
					   &mhash)) )
    {      
#if DEBUG_FS
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Result from datastore filtered by bloomfilter.\n");
#endif
      GNUNET_DATASTORE_get_next (dsh, GNUNET_YES);
      return;
    }
  pgc->results_found++;
  if ( (pgc->type == GNUNET_DATASTORE_BLOCKTYPE_KBLOCK) ||
       (pgc->type == GNUNET_DATASTORE_BLOCKTYPE_SBLOCK) ||
       (pgc->type == GNUNET_DATASTORE_BLOCKTYPE_SKBLOCK) )
    {
      if (pgc->bf == NULL)
	{
	  pgc->bf_size = 32;
	  pgc->bf = GNUNET_CONTAINER_bloomfilter_init (NULL,
						       pgc->bf_size, 
						       BLOOMFILTER_K);
	}
      GNUNET_CONTAINER_bloomfilter_add (pgc->bf, 
					&mhash);
    }

  reply = GNUNET_malloc (sizeof (struct PutMessage) + size);
  reply->header.size = htons (sizeof (struct PutMessage) + size);
  reply->header.type = htons (GNUNET_MESSAGE_TYPE_FS_PUT);
  reply->type = htonl (type);
  reply->expiration = GNUNET_TIME_relative_hton (GNUNET_TIME_absolute_get_remaining (expiration));
  memcpy (&reply[1], data, size);
  GNUNET_CORE_notify_transmit_ready (core,
				     pgc->priority,
				     ACCEPTABLE_REPLY_DELAY,
				     &pgc->reply_to,
				     sizeof (struct PutMessage) + size,
				     &transmit_message,
				     reply);
  if ( (GNUNET_YES == test_load_too_high()) ||
       (pgc->results_found > 5 + 2 * pgc->priority) )
    {
      GNUNET_DATASTORE_get_next (dsh, GNUNET_NO);
      pgc->policy &= ~ ROUTING_POLICY_FORWARD;
      return;
    }
  GNUNET_DATASTORE_get_next (dsh, GNUNET_YES);
}
  

/**
 * We're processing a GET request from another peer.  Give it to our
 * local datastore.
 *
 * @param cls our "struct ProcessGetContext"
 * @param ok did we get a datastore slice or not?
 */
static void
ds_get_request (void *cls, 
		int ok)
{
  struct ProcessGetContext *pgc = cls;
  uint32_t type;
  struct GNUNET_TIME_Relative timeout;

  if (GNUNET_OK != ok)
    {
      /* no point in doing P2P stuff if we can't even do local */
      GNUNET_free (dsh);
      return;
    }
  type = pgc->type;
  if (type == GNUNET_DATASTORE_BLOCKTYPE_DBLOCK)
    type = GNUNET_DATASTORE_BLOCKTYPE_ANY; /* to get on-demand as well */
  timeout = GNUNET_TIME_relative_multiply (BASIC_DATASTORE_REQUEST_DELAY,
					   (pgc->priority + 1));
  GNUNET_DATASTORE_get (dsh,
			&pgc->query,
			type,
			&process_p2p_get_result,
			pgc,
			timeout);
}


/**
 * The priority level imposes a bound on the maximum
 * value for the ttl that can be requested.
 *
 * @param ttl_in requested ttl
 * @param prio given priority
 * @return ttl_in if ttl_in is below the limit,
 *         otherwise the ttl-limit for the given priority
 */
static int32_t
bound_ttl (int32_t ttl_in, uint32_t prio)
{
  unsigned long long allowed;

  if (ttl_in <= 0)
    return ttl_in;
  allowed = ((unsigned long long) prio) * TTL_DECREMENT / 1000; 
  if (ttl_in > allowed)      
    {
      if (allowed >= (1 << 30))
        return 1 << 30;
      return allowed;
    }
  return ttl_in;
}


/**
 * We've received a request with the specified
 * priority.  Bound it according to how much
 * we trust the given peer.
 * 
 * @param prio_in requested priority
 * @param peer the peer making the request
 * @return effective priority
 */
static uint32_t
bound_priority (uint32_t prio_in,
		const struct GNUNET_PeerIdentity *peer)
{
  return 0; // FIXME!
}


/**
 * Handle P2P "GET" request.
 *
 * @param cls closure, always NULL
 * @param other the other peer involved (sender or receiver, NULL
 *        for loopback messages where we are both sender and receiver)
 * @param message the actual message
 * @param latency reported latency of the connection with 'other'
 * @param distance reported distance (DV) to 'other' 
 * @return GNUNET_OK to keep the connection open,
 *         GNUNET_SYSERR to close it (signal serious error)
 */
static int
handle_p2p_get (void *cls,
		const struct GNUNET_PeerIdentity *other,
		const struct GNUNET_MessageHeader *message,
				  struct GNUNET_TIME_Relative latency,
				  uint32_t distance)
{
  uint16_t msize;
  const struct GetMessage *gm;
  unsigned int bits;
  const GNUNET_HashCode *opt;
  struct ProcessGetContext *pgc;
  uint32_t bm;
  size_t bfsize;
  uint32_t ttl_decrement;
  double preference;
  int net_load_up;
  int net_load_down;

  msize = ntohs(message->size);
  if (msize < sizeof (struct GetMessage))
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
  gm = (const struct GetMessage*) message;
  bm = ntohl (gm->hash_bitmap);
  bits = 0;
  while (bm > 0)
    {
      if (1 == (bm & 1))
	bits++;
      bm >>= 1;
    }
  if (msize < sizeof (struct GetMessage) + bits * sizeof (GNUNET_HashCode))
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }  
  opt = (const GNUNET_HashCode*) &gm[1];
  bfsize = msize - sizeof (struct GetMessage) + bits * sizeof (GNUNET_HashCode);
  pgc = GNUNET_malloc (sizeof (struct ProcessGetContext));
  if (bfsize > 0)
    {
      pgc->bf = GNUNET_CONTAINER_bloomfilter_init ((const char*) &pgc[1],
						   bfsize,
						   BLOOMFILTER_K);
      pgc->bf_size = bfsize;
    }
  pgc->type = ntohl (gm->type);
  pgc->bm = ntohl (gm->hash_bitmap);
  pgc->mingle = gm->filter_mutator;
  bits = 0;
  if (0 != (pgc->bm & GET_MESSAGE_BIT_RETURN_TO))
    pgc->reply_to.hashPubKey = opt[bits++];
  else
    pgc->reply_to = *other;
  if (0 != (pgc->bm & GET_MESSAGE_BIT_SKS_NAMESPACE))
    pgc->namespace = opt[bits++];
  else if (pgc->type == GNUNET_DATASTORE_BLOCKTYPE_SBLOCK)
    {
      GNUNET_break_op (0);
      GNUNET_free (pgc);
      return GNUNET_SYSERR;
    }
  if (0 != (pgc->bm & GET_MESSAGE_BIT_TRANSMIT_TO))
    pgc->prime_target.hashPubKey = opt[bits++];
  /* note that we can really only check load here since otherwise
     peers could find out that we are overloaded by being disconnected
     after sending us a malformed query... */
  if (GNUNET_YES == test_load_too_high ())
    {
      if (NULL != pgc->bf)
	GNUNET_CONTAINER_bloomfilter_free (pgc->bf);
      GNUNET_free (pgc);
#if DEBUG_FS
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Dropping query from `%s', this peer is too busy.\n",
		  GNUNET_i2s (other));
#endif
      return GNUNET_OK;
    }
  net_load_up = 50; // FIXME
  net_load_down = 50; // FIXME
  pgc->policy = ROUTING_POLICY_NONE;
  if ( (net_load_up < IDLE_LOAD_THRESHOLD) &&
       (net_load_down < IDLE_LOAD_THRESHOLD) )
    {
      pgc->policy |= ROUTING_POLICY_ALL;
      pgc->priority = 0; /* no charge */
    }
  else
    {
      pgc->priority = bound_priority (ntohl (gm->priority), other);
      if ( (net_load_up < 
	    IDLE_LOAD_THRESHOLD + pgc->priority * pgc->priority) &&
	   (net_load_down < 
	    IDLE_LOAD_THRESHOLD + pgc->priority * pgc->priority) )
	{
	  pgc->policy |= ROUTING_POLICY_ALL;
	}
      else
	{
	  // FIXME: is this sound?
	  if (net_load_up < 90 + 10 * pgc->priority)
	    pgc->policy |= ROUTING_POLICY_FORWARD;
	  if (net_load_down < 90 + 10 * pgc->priority)
	    pgc->policy |= ROUTING_POLICY_ANSWER;
        }
    }
  if (pgc->policy == ROUTING_POLICY_NONE)
    {
#if DEBUG_FS
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Dropping query from `%s', network saturated.\n",
		  GNUNET_i2s (other));
#endif
      if (NULL != pgc->bf)
	GNUNET_CONTAINER_bloomfilter_free (pgc->bf);
      GNUNET_free (pgc);
      return GNUNET_OK;     /* drop */
    }
  if ((pgc->policy & ROUTING_POLICY_INDIRECT) != ROUTING_POLICY_INDIRECT)
    pgc->priority = 0;  /* kill the priority (we cannot benefit) */
  pgc->ttl = bound_ttl (ntohl (gm->ttl), pgc->priority);
  /* decrement ttl (always) */
  ttl_decrement = 2 * TTL_DECREMENT +
    GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK,
			      TTL_DECREMENT);
  if ( (pgc->ttl < 0) &&
       (pgc->ttl - ttl_decrement > 0) )
    {
#if DEBUG_FS
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Dropping query from `%s' due to TTL underflow.\n",
		  GNUNET_i2s (other));
#endif
      /* integer underflow => drop (should be very rare)! */
      if (NULL != pgc->bf)
	GNUNET_CONTAINER_bloomfilter_free (pgc->bf);
      GNUNET_free (pgc);
      return GNUNET_OK;
    }
  pgc->ttl -= ttl_decrement;
  pgc->start_time = GNUNET_TIME_absolute_get ();
  preference = (double) pgc->priority;
  if (preference < QUERY_BANDWIDTH_VALUE)
    preference = QUERY_BANDWIDTH_VALUE;
  // FIXME: also reserve bandwidth for reply?
  (void) GNUNET_CORE_peer_change_preference (sched, cfg,
					     other,
					     GNUNET_TIME_UNIT_FOREVER_REL,
					     0, 0, preference, NULL, NULL);
  if (0 != (pgc->policy & ROUTING_POLICY_ANSWER))
    pgc->drq = queue_ds_request (BASIC_DATASTORE_REQUEST_DELAY,
				 &ds_get_request,
				 pgc);
  else
    GNUNET_SCHEDULER_add_continuation (sched,
				       &forward_get_request,
				       pgc,
				       GNUNET_SCHEDULER_REASON_PREREQ_DONE);
  return GNUNET_OK;
}


/**
 * Function called to notify us that we can now transmit a reply to a
 * client or peer.  "buf" will be NULL and "size" zero if the socket was
 * closed for writing in the meantime.
 *
 * @param cls closure, points to a "struct PendingRequest*" with
 *            one or more pending replies
 * @param size number of bytes available in buf
 * @param buf where the callee should write the message
 * @return number of bytes written to buf
 */
static size_t
transmit_result (void *cls,
		 size_t size, 
		 void *buf)
{
  struct PendingRequest *pr = cls;
  char *cbuf = buf;
  struct PendingMessage *reply;
  size_t ret;

  ret = 0;
  while (NULL != (reply = pr->replies_pending))
    {
      if ( (reply->msize + ret < ret) ||
	   (reply->msize + ret > size) )
	break;
      pr->replies_pending = reply->next;
      memcpy (&cbuf[ret], &reply[1], reply->msize);
      ret += reply->msize;
      GNUNET_free (reply);
    }
  return ret;
}


/**
 * We have received a reply; handle it!
 *
 * @param cls response (struct ProcessReplyClosure)
 * @param key our query
 * @param value value in the hash map (meta-info about the query)
 * @return GNUNET_YES (we should continue to iterate)
 */
static int
process_reply (void *cls,
	       const GNUNET_HashCode * key,
	       void *value)
{
  struct ProcessReplyClosure *prq = cls;
  struct PendingRequest *pr = value;
  struct PendingRequest *eer;
  struct PendingMessage *reply;
  struct PutMessage *pm;
  struct ContentMessage *cm;
  struct ConnectedPeer *cp;
  GNUNET_HashCode chash;
  GNUNET_HashCode mhash;
  struct GNUNET_PeerIdentity target;
  size_t msize;
  uint32_t prio;
  struct GNUNET_TIME_Relative max_delay;
  
  GNUNET_CRYPTO_hash (prq->data,
		      prq->size,
		      &chash);
  switch (prq->type)
    {
    case GNUNET_DATASTORE_BLOCKTYPE_DBLOCK:
    case GNUNET_DATASTORE_BLOCKTYPE_IBLOCK:
      break;
    case GNUNET_DATASTORE_BLOCKTYPE_SBLOCK:
      /* FIXME: does prq->namespace match our expectations? */
      /* then: fall-through??? */
    case GNUNET_DATASTORE_BLOCKTYPE_KBLOCK:
      if (pr->bf != NULL) 
	{
	  mingle_hash (&chash, pr->mingle, &mhash);
	  if (GNUNET_YES == GNUNET_CONTAINER_bloomfilter_test (pr->bf,
							       &mhash))
	    return GNUNET_YES; /* duplicate */
	  GNUNET_CONTAINER_bloomfilter_add (pr->bf,
					    &mhash);
	}
      break;
    case GNUNET_DATASTORE_BLOCKTYPE_SKBLOCK:
      // FIXME: any checks against duplicates for SKBlocks?
      break;
    }
  prio = pr->priority;
  prq->priority += pr->remaining_priority;
  pr->remaining_priority = 0;
  if (pr->client != NULL)
    {
      if (pr->replies_seen_size == pr->replies_seen_off)
	GNUNET_array_grow (pr->replies_seen,
			   pr->replies_seen_size,
			   pr->replies_seen_size * 2 + 4);
      pr->replies_seen[pr->replies_seen_off++] = chash;
      // FIXME: possibly recalculate BF!
    }
  if (pr->client == NULL)
    {
      GNUNET_PEER_resolve (pr->source_pid,
			   &target);
      cp = GNUNET_CONTAINER_multihashmap_get (connected_peers,
					      &target.hashPubKey);
      msize = sizeof (struct ContentMessage) + prq->size;
      reply = GNUNET_malloc (msize + sizeof (struct PendingMessage));
      reply->msize = msize;
      reply->priority = (uint32_t) -1; /* send replies first! */
      cm = (struct ContentMessage*) &reply[1];
      cm->header.type = htons (GNUNET_MESSAGE_TYPE_FS_CONTENT);
      cm->header.size = htons (msize);
      cm->type = htonl (prq->type);
      cm->expiration = GNUNET_TIME_absolute_hton (prq->expiration);
      memcpy (&reply[1], prq->data, prq->size);
      max_delay = GNUNET_TIME_UNIT_FOREVER_REL;
      if (GNUNET_CONTAINER_heap_get_size (requests_by_expiration) >= max_pending_requests)
	{
	  /* estimate expiration time from time difference between
	     first request that will be discarded and this request */
	  eer = GNUNET_CONTAINER_heap_peek (requests_by_expiration);
	  max_delay = GNUNET_TIME_absolute_get_difference (pr->start_time,
							   eer->start_time);
	}

      if (cp == NULL)
	{
	  /* FIXME: bound queue size! */
	  reply->next = pr->replies_pending;
	  pr->replies_pending = reply;
	  if (pr->cth == NULL)
	    {
	      /* implicitly tries to connect */
	      pr->cth = GNUNET_CORE_notify_transmit_ready (core,
							   prio,
							   max_delay,
							   &target,
							   msize,
							   &transmit_result,
							   pr);
	    }
	}
      else
	{
	  /* insert replies always at the head */
	  reply->next = cp->pending_messages;
	  cp->pending_messages = reply;
	  if (cp->cth == NULL)
	    cp->cth = GNUNET_CORE_notify_transmit_ready (core,
							 reply->priority,
							 GNUNET_TIME_UNIT_FOREVER_REL,
							 &target,
							 msize,
							 &transmit_request_cb,
							 cp);
	}
    }
  else
    {
      msize = sizeof (struct PutMessage) + prq->size;
      reply = GNUNET_malloc (msize + sizeof (struct PendingMessage));
      reply->msize = msize;
      reply->next = pr->replies_pending;
      pm = (struct PutMessage*) &reply[1];
      pm->header.type = htons (GNUNET_MESSAGE_TYPE_FS_PUT);
      pm->header.size = htons (msize);
      pm->type = htonl (prq->type);
      pm->expiration = GNUNET_TIME_relative_hton (GNUNET_TIME_absolute_get_remaining (prq->expiration));
      pr->replies_pending = reply;
      memcpy (&reply[1], prq->data, prq->size);
      if (pr->th != NULL)
	return GNUNET_YES;
      pr->th = GNUNET_SERVER_notify_transmit_ready (pr->client,
						    msize,
						    GNUNET_TIME_UNIT_FOREVER_REL,
						    &transmit_result,
						    pr);
      if (pr->th == NULL)
	{
	  // FIXME: need to try again later (not much
	  // to do here specifically, but we need to
	  // check somewhere else to handle this case!)
	}
    }
  // FIXME: implement hot-path routing statistics keeping!
  return GNUNET_YES;
}


/**
 * Check if the given KBlock is well-formed.
 *
 * @param kb the kblock data (or at least "dsize" bytes claiming to be one)
 * @param dsize size of "kb" in bytes; check for < sizeof(struct KBlock)!
 * @param query where to store the query that this block answers
 * @return GNUNET_OK if this is actually a well-formed KBlock
 */
static int
check_kblock (const struct KBlock *kb,
	      size_t dsize,
	      GNUNET_HashCode *query)
{
  if (dsize < sizeof (struct KBlock))
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
  if (dsize - sizeof (struct KBlock) !=
      ntohs (kb->purpose.size) 
      - sizeof (struct GNUNET_CRYPTO_RsaSignaturePurpose) 
      - sizeof(struct GNUNET_CRYPTO_RsaPublicKeyBinaryEncoded) ) 
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
  if (GNUNET_OK !=
      GNUNET_CRYPTO_rsa_verify (GNUNET_SIGNATURE_PURPOSE_FS_KBLOCK,
				&kb->purpose,
				&kb->signature,
				&kb->keyspace)) 
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
  if (query != NULL)
    GNUNET_CRYPTO_hash (&kb->keyspace,
			sizeof (struct GNUNET_CRYPTO_RsaPublicKeyBinaryEncoded),
			query);
  return GNUNET_OK;
}


/**
 * Check if the given SBlock is well-formed.
 *
 * @param sb the sblock data (or at least "dsize" bytes claiming to be one)
 * @param dsize size of "kb" in bytes; check for < sizeof(struct SBlock)!
 * @param query where to store the query that this block answers
 * @param namespace where to store the namespace that this block belongs to
 * @return GNUNET_OK if this is actually a well-formed SBlock
 */
static int
check_sblock (const struct SBlock *sb,
	      size_t dsize,
	      GNUNET_HashCode *query,	
	      GNUNET_HashCode *namespace)
{
  if (dsize < sizeof (struct SBlock))
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
  if (dsize !=
      ntohs (sb->purpose.size) + sizeof (struct GNUNET_CRYPTO_RsaSignature))
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
  if (GNUNET_OK !=
      GNUNET_CRYPTO_rsa_verify (GNUNET_SIGNATURE_PURPOSE_FS_SBLOCK,
				&sb->purpose,
				&sb->signature,
				&sb->subspace)) 
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
  if (query != NULL)
    *query = sb->identifier;
  if (namespace != NULL)
    GNUNET_CRYPTO_hash (&sb->subspace,
			sizeof (struct GNUNET_CRYPTO_RsaPublicKeyBinaryEncoded),
			namespace);
  return GNUNET_OK;
}


/**
 * Handle P2P "PUT" request.
 *
 * @param cls closure, always NULL
 * @param other the other peer involved (sender or receiver, NULL
 *        for loopback messages where we are both sender and receiver)
 * @param message the actual message
 * @param latency reported latency of the connection with 'other'
 * @param distance reported distance (DV) to 'other' 
 * @return GNUNET_OK to keep the connection open,
 *         GNUNET_SYSERR to close it (signal serious error)
 */
static int
handle_p2p_put (void *cls,
		const struct GNUNET_PeerIdentity *other,
		const struct GNUNET_MessageHeader *message,
		struct GNUNET_TIME_Relative latency,
		uint32_t distance)
{
  const struct PutMessage *put;
  uint16_t msize;
  size_t dsize;
  uint32_t type;
  struct GNUNET_TIME_Absolute expiration;
  GNUNET_HashCode query;
  struct ProcessReplyClosure prq;

  msize = ntohs (message->size);
  if (msize < sizeof (struct PutMessage))
    {
      GNUNET_break_op(0);
      return GNUNET_SYSERR;
    }
  put = (const struct PutMessage*) message;
  dsize = msize - sizeof (struct PutMessage);
  type = ntohl (put->type);
  expiration = GNUNET_TIME_relative_to_absolute (GNUNET_TIME_relative_ntoh (put->expiration));

  /* first, validate! */
  switch (type)
    {
    case GNUNET_DATASTORE_BLOCKTYPE_DBLOCK:
    case GNUNET_DATASTORE_BLOCKTYPE_IBLOCK:
      GNUNET_CRYPTO_hash (&put[1], dsize, &query);
      break;
    case GNUNET_DATASTORE_BLOCKTYPE_KBLOCK:
      if (GNUNET_OK !=
	  check_kblock ((const struct KBlock*) &put[1],
			dsize,
			&query))
	return GNUNET_SYSERR;
      break;
    case GNUNET_DATASTORE_BLOCKTYPE_SBLOCK:
      if (GNUNET_OK !=
	  check_sblock ((const struct SBlock*) &put[1],
			dsize,
			&query,
			&prq.namespace))
	return GNUNET_SYSERR;
      break;
    case GNUNET_DATASTORE_BLOCKTYPE_SKBLOCK:
      // FIXME -- validate SKBLOCK!
      GNUNET_break (0);
      return GNUNET_OK;
    default:
      /* unknown block type */
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }

  /* now, lookup 'query' */
  prq.data = (const void*) &put[1];
  prq.size = dsize;
  prq.type = type;
  prq.expiration = expiration;
  prq.priority = 0;
  GNUNET_CONTAINER_multihashmap_get_multiple (requests_by_query,
					      &query,
					      &process_reply,
					      &prq);
  // FIXME: if migration is on and load is low,
  // queue to store data in datastore;
  // use "prq.priority" for that!
  return GNUNET_OK;
}


/**
 * List of handlers for P2P messages
 * that we care about.
 */
static struct GNUNET_CORE_MessageHandler p2p_handlers[] =
  {
    { &handle_p2p_get, 
      GNUNET_MESSAGE_TYPE_FS_GET, 0 },
    { &handle_p2p_put, 
      GNUNET_MESSAGE_TYPE_FS_PUT, 0 },
    { NULL, 0, 0 }
  };


/**
 * Process fs requests.
 *
 * @param cls closure
 * @param s scheduler to use
 * @param server the initialized server
 * @param c configuration to use
 */
static void
run (void *cls,
     struct GNUNET_SCHEDULER_Handle *s,
     struct GNUNET_SERVER_Handle *server,
     const struct GNUNET_CONFIGURATION_Handle *c)
{
  sched = s;
  cfg = c;

  requests_by_query = GNUNET_CONTAINER_multihashmap_create (128); // FIXME: get size from config
  requests_by_peer = GNUNET_CONTAINER_multihashmap_create (128); // FIXME: get size from config
  connected_peers = GNUNET_CONTAINER_multihashmap_create (64);
  requests_by_expiration = GNUNET_CONTAINER_heap_create (GNUNET_CONTAINER_HEAP_ORDER_MIN); 
  GNUNET_FS_init_indexing (sched, cfg);
  dsh = GNUNET_DATASTORE_connect (cfg,
				  sched);
  core = GNUNET_CORE_connect (sched,
			      cfg,
			      GNUNET_TIME_UNIT_FOREVER_REL,
			      NULL,
			      NULL,
			      NULL,
			      &peer_connect_handler,
			      &peer_disconnect_handler,
			      NULL, GNUNET_NO,
			      NULL, GNUNET_NO,
			      p2p_handlers);

  GNUNET_SERVER_disconnect_notify (server, 
				   &handle_client_disconnect,
				   NULL);
  GNUNET_SERVER_add_handlers (server, handlers);
  GNUNET_SCHEDULER_add_delayed (sched,
				GNUNET_TIME_UNIT_FOREVER_REL,
				&shutdown_task,
				NULL);
  if (NULL == dsh)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
		  _("Failed to connect to `%s' service.\n"),
		  "datastore");
      GNUNET_SCHEDULER_shutdown (sched);
      return;
    }
  if (NULL == core)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
		  _("Failed to connect to `%s' service.\n"),
		  "core");
      GNUNET_SCHEDULER_shutdown (sched);
      return;
    }
}


/**
 * The main function for the fs service.
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc, char *const *argv)
{
  return (GNUNET_OK ==
          GNUNET_SERVICE_run (argc,
                              argv,
                              "fs",
			      GNUNET_SERVICE_OPTION_NONE,
			      &run, NULL)) ? 0 : 1;
}

/* end of gnunet-service-fs.c */

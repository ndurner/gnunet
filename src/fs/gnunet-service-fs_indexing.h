/*
     This file is part of GNUnet.
     (C) 2009, 2010 Christian Grothoff (and other contributing authors)

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
 * @file fs/gnunet-service-fs_indexing.h
 * @brief indexing for the file-sharing service
 * @author Christian Grothoff
 */
#ifndef GNUNET_SERVICE_FS_INDEXING_H
#define GNUNET_SERVICE_FS_INDEXING_H

#include "gnunet_core_service.h"
#include "gnunet_datastore_service.h"
#include "gnunet_peer_lib.h"
#include "gnunet_protocols.h"
#include "gnunet_signatures.h"
#include "gnunet_util_lib.h"


/**
 * We've received an on-demand encoded block from the datastore.
 * Attempt to do on-demand encoding and (if successful), call the
 * continuation with the resulting block.  On error, clean up and ask
 * the datastore for more results.
 *
 * @param key key for the content
 * @param size number of bytes in data
 * @param data content stored
 * @param type type of the content
 * @param priority priority of the content
 * @param anonymity anonymity-level for the content
 * @param expiration expiration time for the content
 * @param uid unique identifier for the datum;
 *        maybe 0 if no unique identifier is available
 * @param dsh connection to the datastore (to ask for more)
 * @param cont function to call with the actual block
 * @param cont_cls closure for cont
 */
void
GNUNET_FS_handle_on_demand_block (const GNUNET_HashCode * key,
				  uint32_t size,
				  const void *data,
				  uint32_t type,
				  uint32_t priority,
				  uint32_t anonymity,
				  struct GNUNET_TIME_Absolute
				  expiration, uint64_t uid,
				  struct GNUNET_DATASTORE_Handle *dsh,
				  GNUNET_DATASTORE_Iterator cont,
				  void *cont_cls);

/**
 * Handle INDEX_START-message.
 *
 * @param cls closure
 * @param client identification of the client
 * @param message the actual message
 */
void
GNUNET_FS_handle_index_start (void *cls,
			      struct GNUNET_SERVER_Client *client,
			      const struct GNUNET_MessageHeader *message);


/**
 * Handle INDEX_LIST_GET-message.
 *
 * @param cls closure
 * @param client identification of the client
 * @param message the actual message
 */
void
GNUNET_FS_handle_index_list_get (void *cls,
				 struct GNUNET_SERVER_Client *client,
				 const struct GNUNET_MessageHeader *message);


/**
 * Handle UNINDEX-message.
 *
 * @param cls closure
 * @param client identification of the client
 * @param message the actual message
 */
void
GNUNET_FS_handle_unindex (void *cls,
			  struct GNUNET_SERVER_Client *client,
			  const struct GNUNET_MessageHeader *message);




/**
 * Initialize the indexing submodule.
 *
 * @param s scheduler to use
 * @param c configuration to use
 */
void
GNUNET_FS_init_indexing (struct GNUNET_SCHEDULER_Handle *s,
			 const struct GNUNET_CONFIGURATION_Handle *c);


#endif

/*
     This file is part of GNUnet.
     (C) 2013 Christian Grothoff (and other contributing authors)

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
     Free Software Foundation, Inc., 59 Temple Place - Suite 330,
     Boston, MA 02111-1307, USA.
*/

/**
 * @file mesh/gnunet-service-mesh_channel.h
 * @brief mesh service; dealing with end-to-end channels
 * @author Bartlomiej Polot
 *
 * All functions in this file should use the prefix GMCH (Gnunet Mesh CHannel)
 */

#ifndef GNUNET_SERVICE_MESH_CHANNEL_H
#define GNUNET_SERVICE_MESH_CHANNEL_H

#ifdef __cplusplus
extern "C"
{
#if 0                           /* keep Emacsens' auto-indent happy */
}
#endif
#endif

#include "platform.h"
#include "gnunet_util_lib.h"

/**
 * Struct containing all information regarding a channel to a remote client.
 */
struct MeshChannel;


#include "gnunet-service-mesh_tunnel.h"


/**
 * Get channel ID.
 *
 * @param ch Channel.
 *
 * @return ID
 */
MESH_ChannelNumber
GMCH_get_id (const struct MeshChannel *ch);

/**
 * Get the channel tunnel.
 *
 * @param ch Channel to get the tunnel from.
 *
 * @return tunnel of the channel.
 */
struct MeshTunnel3 *
GMCH_get_tunnel (const struct MeshChannel *ch);

/**
 * Get free buffer space towards the client on a specific channel.
 *
 * @param ch Channel.
 * @param fwd Is query about FWD traffic?
 *
 * @return Free buffer space [0 - 64]
 */
unsigned int
GMCH_get_buffer (struct MeshChannel *ch, int fwd);

/**
 * Is the root client for this channel on this peer?
 *
 * @param ch Channel.
 * @param fwd Is this for fwd traffic?
 *
 * @return GNUNET_YES in case it is.
 */
int
GMCH_is_origin (struct MeshChannel *ch, int fwd);

/**
 * Is the destination client for this channel on this peer?
 *
 * @param ch Channel.
 * @param fwd Is this for fwd traffic?
 *
 * @return GNUNET_YES in case it is.
 */
int
GMCH_is_terminal (struct MeshChannel *ch, int fwd);

/**
 * Send an end-to-end ACK message for the most recent in-sequence payload.
 *
 * If channel is not reliable, do nothing.
 *
 * @param ch Channel this is about.
 * @param fwd Is for FWD traffic? (ACK dest->owner)
 */
void
GMCH_send_ack (struct MeshChannel *ch, int fwd);

/**
 * Send data on a channel.
 *
 * If the destination is local, send it to client, otherwise encrypt and
 * send to next hop.
 *
 * @param ch Channel
 * @param msg Message.
 * @param fwd Is this a fwd (root->dest) message?
 */
void
GMCH_send_data (struct MeshChannel *ch,
                const struct GNUNET_MESH_Data *msg,
                int fwd);

/**
 * Notify the destination client that a new incoming channel was created.
 *
 * @param ch Channel that was created.
 */
void
GMCH_send_create (struct MeshChannel *ch);


/**
 * Notify a client that the channel is no longer valid.
 *
 * @param ch Channel that is destroyed.
 * @param fwd Forward notification (owner->dest)?
 */
void
GMCH_send_destroy (struct MeshChannel *ch, int fwd);


/**
 * Log channel info.
 *
 * @param ch Channel.
 */
void
GMCH_debug (struct MeshChannel *ch);


/**
 * Handler for mesh network payload traffic.
 *
 * @param ch Channel for the message.
 * @param message Unencryted data message.
 * @param fwd Is this FWD traffic? GNUNET_YES : GNUNET_NO;
 */
void
GMCH_handle_data (struct MeshChannel *ch,
                  const struct GNUNET_MESH_Data *msg,
                  int fwd);


/**
 * Handler for mesh network traffic end-to-end ACKs.
 *
 * @param t Tunnel on which we got this message.
 * @param message Data message.
 * @param fwd Is this a fwd ACK? (dest->orig)
 */
void
GMCH_handle_data_ack (struct MeshChannel *ch,
                      const struct GNUNET_MESH_DataACK *msg,
                      int fwd);


/**
 * Handler for channel create messages.
 *
 * @param t Tunnel this channel is to be created in.
 * @param msg Message.
 * @param fwd Is this FWD traffic? GNUNET_YES : GNUNET_NO;
 */
struct MeshChannel *
GMCH_handle_create (const struct GNUNET_MESH_ChannelCreate *msg,
                    int fwd);


/**
 * Handler for channel ack messages.
 *
 * @param t Tunnel this channel is to be created in.
 * @param msg Message.
 * @param fwd Is this FWD traffic? GNUNET_YES : GNUNET_NO;
 */
void
GMCH_handle_ack (struct MeshChannel *ch,
                 const struct GNUNET_MESH_ChannelManage *msg,
                 int fwd);


/**
 * Handler for channel destroy messages.
 *
 * @param t Tunnel this channel is to be destroyed of.
 * @param msg Message.
 * @param fwd Is this FWD traffic? GNUNET_YES : GNUNET_NO;
 */
void
GMCH_handle_destroy (struct MeshChannel *ch,
                     const struct GNUNET_MESH_ChannelManage *msg,
                     int fwd);



/**
 * Sends an already built message on a channel, properly registering
 * all used resources and encrypting the message with the tunnel's key.
 *
 * @param message Message to send. Function makes a copy of it.
 * @param ch Channel on which this message is transmitted.
 * @param fwd Is this a fwd message?
 */
void
GMCH_send_prebuilt_message (const struct GNUNET_MessageHeader *message,
                            struct MeshChannel *ch, int fwd);


#if 0                           /* keep Emacsens' auto-indent happy */
{
#endif
#ifdef __cplusplus
}
#endif

/* ifndef GNUNET_SERVICE_MESH_CHANNEL_H */
#endif
/* end of gnunet-service-mesh_channel.h */

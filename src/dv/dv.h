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
 * @author Christian Grothoff
 * @file dv/dv.h
 * @brief IPC messages between DV service and DV plugin
 */
#ifndef DV_H
#define DV_H

#include "gnunet_common.h"

GNUNET_NETWORK_STRUCT_BEGIN

/**
 * DV service tells plugin about a DV-connection being
 * now available.
 */
struct GNUNET_DV_ConnectMessage
{
  /**
   * Type:  GNUNET_MESSAGE_TYPE_TRANSPORT_DV_CONNECT
   */
  struct GNUNET_MessageHeader header;

  /**
   * The distance to the peer that we are now connected to
   */
  uint32_t distance GNUNET_PACKED;

  /**
   * The other peer (at the given distance).
   */
  struct GNUNET_PeerIdentity peer;

};


/**
 * DV service tells plugin about a DV-connection being
 * no longer available.
 *
 * Sender address is copied to the end of this struct,
 * followed by the actual message received.
 */
struct GNUNET_DV_DisconnectMessage
{
  /**
   * Type:  GNUNET_MESSAGE_TYPE_TRANSPORT_DV_DISCONNECT
   */
  struct GNUNET_MessageHeader header;

  /**
   * The distance to the peer that we used to have
   */
  uint32_t distance GNUNET_PACKED;

  /**
   * The peer that is no longer available.
   */
  struct GNUNET_PeerIdentity peer;

};


/**
 * DV Message, contains a message that was received via DV for this
 * peer.  Send from the DV service to the DV plugin.
 *
 * Sender address is copied to the end of this struct,
 * followed by the actual message received.
 */
struct GNUNET_DV_ReceivedMessage
{
  /**
   * Type:  GNUNET_MESSAGE_TYPE_TRANSPORT_DV_RECV
   */
  struct GNUNET_MessageHeader header;

  /**
   * The distance to the peer that we received the message from
   */
  uint32_t distance GNUNET_PACKED;

  /**
   * The (actual) sender of the message
   */
  struct GNUNET_PeerIdentity sender;

  /* payload follows */
};


/**
 * Message from plugin to DV service, requesting a
 * message to be routed.
 */
struct GNUNET_DV_SendMessage
{
  /**
   * Type: GNUNET_MESSAGE_TYPE_DV_SEND
   */
  struct GNUNET_MessageHeader header;

  /**
   * Unique ID for this message, for confirm callback.
   */
  uint32_t uid GNUNET_PACKED;

  /**
   * The (actual) target of the message
   */
  struct GNUNET_PeerIdentity target;

};


/**
 * Message from service to DV plugin, saying that a
 * SEND request was handled.  
 */
struct GNUNET_DV_AckMessage
{
  /**
   * Type: GNUNET_MESSAGE_TYPE_DV_SEND_ACK
   */ 
  struct GNUNET_MessageHeader header;

  /**
   * Which message is being acknowledged?
   */
  uint32_t uid GNUNET_PACKED;

  /**
   * The (actual) target of the message
   */
  struct GNUNET_PeerIdentity target;

};
GNUNET_NETWORK_STRUCT_END

#endif

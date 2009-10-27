// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2009 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#include "CephxServiceHandler.h"
#include "CephxProtocol.h"

#include "../Auth.h"

#include "mon/Monitor.h"

#include <errno.h>
#include <sstream>

#include "config.h"

#define DOUT_SUBSYS auth
#undef dout_prefix
#define dout_prefix *_dout << dbeginl << "cephx server " << entity_name << ": "

int CephxServiceHandler::start_session(bufferlist& result_bl)
{
  get_random_bytes((char *)&server_challenge, sizeof(server_challenge));
  if (!server_challenge)
    server_challenge = 1;  // always non-zero.
  dout(10) << "start_session server_challenge " << hex << server_challenge << dec << dendl;

  CephXServerChallenge ch;
  ch.server_challenge = server_challenge;
  ::encode(ch, result_bl);
  return CEPH_AUTH_CEPHX;
}

int CephxServiceHandler::handle_request(bufferlist::iterator& indata, bufferlist& result_bl, bufferlist& caps)
{
  int ret = 0;

  struct CephXRequestHeader cephx_header;
  ::decode(cephx_header, indata);

  switch (cephx_header.request_type) {
  case CEPHX_GET_AUTH_SESSION_KEY:
    {
      CephXAuthenticate req;
      ::decode(req, indata);

      entity_name = req.name;

      CryptoKey secret;
      dout(10) << "handle_request get_auth_session_key for " << entity_name << dendl;
      if (!key_server->get_secret(entity_name, secret)) {
        dout(0) << "couldn't find entity name: " << entity_name << dendl;
	ret = -EPERM;
	break;
      }

      if (!server_challenge) {
	ret = -EPERM;
	break;
      }      
      bufferlist key, key_enc;
      ::encode(server_challenge, key);
      ::encode(req.client_challenge, key);
      ret = encode_encrypt(key, secret, key_enc);
      if (ret < 0)
        break;
      uint64_t expected_key = 0;
      const uint64_t *p = (const uint64_t *)key_enc.c_str();
      for (int pos = 0; pos + sizeof(req.key) <= key_enc.length(); pos+=sizeof(req.key), p++) {
        expected_key ^= *p;
      }
      dout(0) << " checking key: req.key=" << hex << req.key << " expected_key=" << expected_key << dec << dendl;
      if (req.key != expected_key) {
        dout(0) << " unexpected key: req.key=" << req.key << " expected_key=" << expected_key << dendl;
        ret = -EPERM;
	break;
      }

      CryptoKey session_key;
      CephXSessionAuthInfo info;

      CryptoKey principal_secret;
      if (key_server->get_secret(req.name, principal_secret) < 0) {
	ret = -EPERM;
	break;
      }

      info.ticket.name = req.name;
      info.ticket.init_timestamps(g_clock.now(), g_conf.auth_mon_ticket_ttl);

      key_server->generate_secret(session_key);

      info.session_key = session_key;
      info.service_id = CEPH_ENTITY_TYPE_AUTH;
      if (!key_server->get_service_secret(CEPH_ENTITY_TYPE_AUTH, info.service_secret, info.secret_id)) {
        dout(0) << " could not get service secret for auth subsystem" << dendl;
        ret = -EIO;
        break;
      }

      vector<CephXSessionAuthInfo> info_vec;
      info_vec.push_back(info);

      build_cephx_response_header(cephx_header.request_type, 0, result_bl);
      if (!cephx_build_service_ticket_reply(principal_secret, info_vec, result_bl)) {
        ret = -EIO;
        break;
      }

      if (!key_server->get_service_caps(entity_name, CEPH_ENTITY_TYPE_MON, caps)) {
        dout(0) << " could not get mon caps for " << entity_name << dendl;
      }
    }
    break;

  case CEPHX_GET_PRINCIPAL_SESSION_KEY:
    {
      dout(10) << "handle_request get_principal_session_key" << dendl;

      bufferlist tmp_bl;
      CephXServiceTicketInfo auth_ticket_info;
      if (!cephx_verify_authorizer(*key_server, indata, auth_ticket_info, tmp_bl)) {
        ret = -EPERM;
      }

      CephXServiceTicketRequest ticket_req;
      ::decode(ticket_req, indata);
      dout(10) << " ticket_req.keys = " << ticket_req.keys << dendl;

      ret = 0;
      vector<CephXSessionAuthInfo> info_vec;
      for (uint32_t service_id = 1; service_id <= ticket_req.keys; service_id <<= 1) {
        if (ticket_req.keys & service_id) {
	  dout(10) << " adding key for service " << service_id << dendl;
          CephXSessionAuthInfo info;
          int r = key_server->build_session_auth_info(service_id, auth_ticket_info, info);
          if (r < 0) {
            ret = r;
            break;
          }
          info_vec.push_back(info);
        }
      }
      build_cephx_response_header(cephx_header.request_type, ret, result_bl);
      cephx_build_service_ticket_reply(auth_ticket_info.session_key, info_vec, result_bl);
    }
    break;

  case CEPHX_GET_ROTATING_KEY:
    {
      dout(10) << "handle_request getting rotating secret for " << entity_name << dendl;
      build_cephx_response_header(cephx_header.request_type, 0, result_bl);
      key_server->get_rotating_encrypted(entity_name, result_bl);
      ret = 0;
    }
    break;

  default:
    dout(10) << "handle_request unkonwn op " << cephx_header.request_type << dendl;
    return -EINVAL;
  }
  return ret;
}

void CephxServiceHandler::build_cephx_response_header(int request_type, int status, bufferlist& bl)
{
  struct CephXResponseHeader header;
  header.request_type = request_type;
  header.status = status;
  ::encode(header, bl);
}

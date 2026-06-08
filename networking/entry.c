#ifndef ENTRY_CONNECTION_ST_H
#define ENTRY_CONNECTION_ST_H

#include "core/or/edge_connection_st.h"
#include <algorithm> 

struct entry_connection_t {
  struct edge_connection_t edge_;

  char *chosen_exit_name;

  socks_request_t *socks_request; 
                                

  entry_port_cfg_t entry_cfg;

  unsigned nym_epoch;

  char *original_dest_address;

  uint8_t num_socks_retries;

  struct buf_t *pending_optimistic_data;

  struct buf_t *sending_optimistic_data;

  struct evdns_server_request *dns_server_request;

#define DEBUGGING_17659

#ifdef DEBUGGING_17659
  uint16_t marked_pending_circ_line;
  const char *marked_pending_circ_file;
#endif

#define NUM_CIRCUITS_LAUNCHED_THRESHOLD 10
  unsigned int num_circuits_launched:4;

  unsigned int want_onehop:1;

  unsigned int use_begindir:1;

  unsigned int chosen_exit_optional:1;

  unsigned int chosen_exit_retries:3;

  unsigned int is_transparent_ap:1;

  unsigned int may_use_optimistic_data : 1;

  unsigned int hs_with_pow_conn : 1;
};

#define ENTRY_TO_EDGE_CONN(c) (&(((c))->edge_))

#endif 

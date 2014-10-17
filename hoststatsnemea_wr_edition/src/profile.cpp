/*
 * Copyright (C) 2013 CESNET
 *
 * LICENSE TERMS
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#include "profile.h"
#include "aux_func.h"
#include "processdata.h"
#include <limits>

using namespace std;

extern uint32_t hs_time;

// DEFAULT VALUES OF THE MAIN PROFILE (in seconds)
#define DEF_SIZE (65536)
#define D_ACTIVE_TIMEOUT   300   // default value (in seconds)
#define D_INACTIVE_TIMEOUT 30    // default value (in seconds)
#define D_DET_START_PAUSE  5     // default time between starts of detector (in seconds)

#define STAT_TABLE_STASH_SIZE 4

/* -------------------- MAIN PROFILE -------------------- */

HostProfile::HostProfile()
{
   // Fill vector of subprofiles 
   subprofile_t sp_dns("dns", "rules-dns", dns_pointers);
   sp_list.push_back(sp_dns);

   subprofile_t sp_ssh("ssh", "rules-ssh", ssh_pointers);
   sp_list.push_back(sp_ssh);

   // Load configuration
   apply_config();

   // Initialization of hosts stats table
   stat_table = fht_init(table_size / 4, sizeof(hosts_key_t), 
      sizeof(hosts_record_t), STAT_TABLE_STASH_SIZE);
   if (stat_table == NULL) {
      log(LOG_CRIT, "CRITICAL ERROR: Failed to initialize the statistics table.");
      exit(1);
   }
   
   // Create BloomFilters
   bloom_parameters bp;
   bp.projected_element_count = 2 * table_size;
   bp.false_positive_probability = 0.01;
   bp.compute_optimal_parameters();
   log(LOG_DEBUG, "process_data: Creating Bloom Filter, table size: %d, hashes: %d",
      bp.optimal_parameters.table_size, bp.optimal_parameters.number_of_hashes);
   bf_active = new bloom_filter(bp);
   bf_learn = new bloom_filter(bp);
}

HostProfile::~HostProfile()
{
   // Remove all subprofiles
   release();

   // Delete hosts stats table
   fht_destroy(stat_table);

   // Delete BloomFilters
   delete bf_active;
   delete bf_learn;

   // Delete all BloomFilters in subprofiles
   for(sp_list_iter it = sp_list.begin(); it != sp_list.end(); ++it) {
      if (it->pointers.bf_config_ptr != NULL) {
         it->pointers.bf_config_ptr(BF_DESTROY, 0);
      }
   }
}

/** \brief Load configuration
 * Load configuration data and update profile (and subprofiles) variables 
 */
void HostProfile::apply_config()
{
   Configuration *conf = Configuration::getInstance();
   conf->lock();
   table_size =       atoi(conf->getValue("table-size").c_str());
   active_timeout =   atoi(conf->getValue("timeout-active").c_str());
   inactive_timeout = atoi(conf->getValue("timeout-inactive").c_str());
   det_start_time =   atoi(conf->getValue("det_start_time").c_str());
   detector_status =  (conf->getValue("rules-generic") == "1"); 
   port_flowdir =     (conf->getValue("port-flowdir") == "1");
   conf->unlock();

   // check size of table
   if (table_size < DEF_SIZE) {
      table_size       = DEF_SIZE;
      log(LOG_WARNING, "Warning: Table size is not specified or value is less "
         "than minimal value. Using %d as default.", DEF_SIZE);
   }
   
   // find the smallest power of two that is greater or equal to a given value
   int i = table_size;
   --i;
   i |= i >> 1;
   i |= i >> 2;
   i |= i >> 4;
   i |= i >> 8;
   i |= i >> 16;
   ++i;
   
   if (i != table_size) {
      table_size = i;
      log(LOG_WARNING, "Warning: Table size is not power of two. Using the "
         "smallest correct value (%d) greater then specified size.", table_size);
   }
   
   // check configuration
   if (det_start_time <= 0) {
      det_start_time   = D_DET_START_PAUSE;
      log(LOG_WARNING, "Warning: Detector start time is not specified. Using "
         "%d as default.", D_DET_START_PAUSE);
   }
   if (active_timeout <= 0) {
      active_timeout   = D_ACTIVE_TIMEOUT;
      log(LOG_WARNING, "Warning: Active timeout is not specified. Using "
         "%d as default.", D_ACTIVE_TIMEOUT);
   }
   if (inactive_timeout <= 0) {
      inactive_timeout = D_INACTIVE_TIMEOUT;
      log(LOG_WARNING, "Warning: Active timeout is not specified. Using "
         "%d as default.", D_INACTIVE_TIMEOUT);
   }

   // update subprofile configuration
   for(sp_list_iter it = sp_list.begin(); it != sp_list.end(); ++it) {
      it->check_config();

      if (it->pointers.bf_config_ptr == NULL) {
         continue;
      }

      // Create subprofile's BloomFilters
      if (it->sp_status) {
         it->pointers.bf_config_ptr(BF_CREATE, 2 * table_size);
      }
   }

   // Sort vector - active subprofiles go to the forefront of sp_list
   std::sort(sp_list.begin(), sp_list.end());
}

/** \brief Update records in the stat_table by data from TRAP input interface
 *
 * Find two host records according to source and destination address of the 
 * flow and update these records (and subprofiles).
 * Note: uses global variable hs_time with current system time 
 *
 * \param record Pointer to the data from the TRAP
 * \param tmpl_in Pointer to the TRAP input interface
 * \param subprofiles When True update active subprofiles
 */
void HostProfile::update(const void *record, const ur_template_t *tmpl_in,
      bool subprofiles)
{
   // basic filter for fragments of flows 
   if (ur_get(tmpl_in, record, UR_SRC_PORT) == 0 &&
       ur_get(tmpl_in, record, UR_DST_PORT) == 0 &&
       ur_get(tmpl_in, record, UR_PROTOCOL) == 17) {
      // Skip UDP flows with source and destination port "0"
      return;
   }
   
   // create key for the BloomFilter
   bloom_key_t bloom_key;
   bloom_key.src_ip = ur_get(tmpl_in, record, UR_SRC_IP);
   bloom_key.dst_ip = ur_get(tmpl_in, record, UR_DST_IP);
   
   // common part
   uint8_t tcp_flags = ur_get(tmpl_in, record, UR_TCP_FLAGS);

   uint8_t dir_flags;
   if (port_flowdir) {
      // ignore flowdir from record and create custom
      dir_flags = UR_DIR_FLAG_NRC;
      uint16_t src_port = ur_get(tmpl_in, record, UR_SRC_PORT);
      uint16_t dst_port = ur_get(tmpl_in, record, UR_DST_PORT);

      if (((src_port < 10000) || (dst_port < 10000)) && (src_port != dst_port)) {
         if (src_port < dst_port) {
            dir_flags = UR_DIR_FLAG_RSP;
         } else {
            dir_flags = UR_DIR_FLAG_REQ;
         }
      }
   } else {
      // use flowdir from unirec
      dir_flags = ur_get(tmpl_in, record, UR_DIRECTION_FLAGS);
   }
   
   // Macros
   #define ADD(dst, src) \
      dst = safe_add(dst, src);

   #define INC(value) \
      value = safe_inc(value);
   
   /////////////////////////////////////////////////////////////////////////////
   // SOURCE IP
   
   // get source record and set/update timestamps
   int8_t *src_lock = NULL;
   hosts_record_t& src_host_rec = get_record(bloom_key.src_ip, &src_lock);
   if (!src_host_rec.in_all_flows && !src_host_rec.out_all_flows) {
      src_host_rec.first_rec_ts = hs_time;
   }
   src_host_rec.last_rec_ts = hs_time;
   
   // find/add records in the BloomFilters (get info about presence of this flow)
   bool src_present = false;  
   
   // Items inserted in this BloomFilter use MSB of timestamp to determine
   // record origin (inserted by source IP or destination IP)
   // Presence for source IP
   // |    source IP    | destination IP | first record timestamp |
   // | -- (src_ip) --  | -- (dst_ip) -- |  0XXX XXXX XXXX XXXX   |
   bloom_key.rec_time = src_host_rec.first_rec_ts % (std::numeric_limits<uint16_t>::max() + 1);
   bloom_key.rec_time &= ((1 << 15) - 1);
   src_present = bf_active->containsinsert((const unsigned char *) &bloom_key, 
      sizeof(bloom_key_t));
   bf_learn->insert((const unsigned char *) &bloom_key, sizeof(bloom_key_t));   
   
   // UPDATE STATS
   // all flows
   ADD(src_host_rec.out_all_bytes, ur_get(tmpl_in, record, UR_BYTES));
   ADD(src_host_rec.out_all_packets, ur_get(tmpl_in, record, UR_PACKETS));
   if (!src_present) INC(src_host_rec.out_all_uniqueips);
   INC(src_host_rec.out_all_flows);

   if (tcp_flags & 0x1)  INC(src_host_rec.out_all_fin_cnt);
   if (tcp_flags & 0x2)  INC(src_host_rec.out_all_syn_cnt);
   if (tcp_flags & 0x4)  INC(src_host_rec.out_all_rst_cnt);
   if (tcp_flags & 0x8)  INC(src_host_rec.out_all_psh_cnt);
   if (tcp_flags & 0x10) INC(src_host_rec.out_all_ack_cnt);
   if (tcp_flags & 0x20) INC(src_host_rec.out_all_urg_cnt);

   src_host_rec.out_linkbitfield |= ur_get(tmpl_in, record, UR_LINK_BIT_FIELD);

   if (dir_flags & UR_DIR_FLAG_REQ) {
      // request flows
      ADD(src_host_rec.out_req_bytes, ur_get(tmpl_in, record, UR_BYTES));
      ADD(src_host_rec.out_req_packets, ur_get(tmpl_in, record, UR_PACKETS));
      if (!src_present) INC(src_host_rec.out_req_uniqueips);
      INC(src_host_rec.out_req_flows);

      if (tcp_flags & 0x2)  INC(src_host_rec.out_req_syn_cnt);
      if (tcp_flags & 0x4)  INC(src_host_rec.out_req_rst_cnt);
      if (tcp_flags & 0x8)  INC(src_host_rec.out_req_psh_cnt);
      if (tcp_flags & 0x10) INC(src_host_rec.out_req_ack_cnt);
   } else if (dir_flags & UR_DIR_FLAG_RSP) {
      // response flows
      ADD(src_host_rec.out_rsp_packets, ur_get(tmpl_in, record, UR_PACKETS));
      INC(src_host_rec.out_rsp_flows);

      if (tcp_flags & 0x2)  INC(src_host_rec.out_rsp_syn_cnt);
      if (tcp_flags & 0x10) INC(src_host_rec.out_rsp_ack_cnt);
   }
   
   if (subprofiles) {
      // update subprofiles
      for(sp_list_iter it = sp_list.begin(); it != sp_list.end(); ++it) {
         if (!it->sp_status)
            break;

         it->pointers.update_src_ptr(&bloom_key, src_host_rec, record, tmpl_in,
            dir_flags);
      }      
   }
   
   fht_unlock_data(src_lock);
   
   /////////////////////////////////////////////////////////////////////////////
   // DESTINATION IP
   
   int8_t *dst_lock = NULL;
   hosts_record_t& dst_host_rec = get_record(bloom_key.dst_ip, &dst_lock);
   if (!dst_host_rec.in_all_flows && !dst_host_rec.out_all_flows) {
      dst_host_rec.first_rec_ts = hs_time;
   }
   
   dst_host_rec.last_rec_ts = hs_time;
   
   // find/add record in the BloomFilters (get info about presence of this flow)
   bool dst_present = false;
   
   // Presence for destination IP
   // |    source IP    | destination IP | first record timestamp |
   // | -- (src_ip) --  | -- (dst_ip) -- |  1XXX XXXX XXXX XXXX   |
   bloom_key.rec_time = dst_host_rec.first_rec_ts % (std::numeric_limits<uint16_t>::max() + 1);
   bloom_key.rec_time |= (1 << 15);
   dst_present = bf_active->containsinsert((const unsigned char *) &bloom_key, 
      sizeof(bloom_key_t));
   bf_learn->insert((const unsigned char *) &bloom_key, sizeof(bloom_key_t));

   // UPDATE STATS
   // all flows
   ADD(dst_host_rec.in_all_bytes, ur_get(tmpl_in, record, UR_BYTES));
   ADD(dst_host_rec.in_all_packets, ur_get(tmpl_in, record, UR_PACKETS));
   if (!dst_present) INC(dst_host_rec.in_all_uniqueips);
   INC(dst_host_rec.in_all_flows);

   if (tcp_flags & 0x1)  INC(dst_host_rec.in_all_fin_cnt);
   if (tcp_flags & 0x2)  INC(dst_host_rec.in_all_syn_cnt);
   if (tcp_flags & 0x4)  INC(dst_host_rec.in_all_rst_cnt);
   if (tcp_flags & 0x8)  INC(dst_host_rec.in_all_psh_cnt);
   if (tcp_flags & 0x10) INC(dst_host_rec.in_all_ack_cnt);
   if (tcp_flags & 0x20) INC(dst_host_rec.in_all_urg_cnt);

   dst_host_rec.in_linkbitfield |= ur_get(tmpl_in, record, UR_LINK_BIT_FIELD);

   if (dir_flags & UR_DIR_FLAG_REQ) {
      // request flows
      ADD(dst_host_rec.in_req_bytes, ur_get(tmpl_in, record, UR_BYTES));
      ADD(dst_host_rec.in_req_packets, ur_get(tmpl_in, record, UR_PACKETS));
      if (!dst_present) INC(dst_host_rec.in_req_uniqueips);
      INC(dst_host_rec.in_req_flows);

      if (tcp_flags & 0x4)  INC(dst_host_rec.in_req_rst_cnt);
      if (tcp_flags & 0x8)  INC(dst_host_rec.in_req_psh_cnt);
      if (tcp_flags & 0x10) INC(dst_host_rec.in_req_ack_cnt);
   } else if (dir_flags & UR_DIR_FLAG_RSP) {
      // response flows
      ADD(dst_host_rec.in_rsp_packets, ur_get(tmpl_in, record, UR_PACKETS));
      INC(dst_host_rec.in_rsp_flows);

      if (tcp_flags & 0x10) INC(dst_host_rec.in_rsp_ack_cnt);
   }
   
   if (subprofiles) {
      // update subprofiles
      for(sp_list_iter it = sp_list.begin(); it != sp_list.end(); ++it) {
         if (!it->sp_status)
            break;

         it->pointers.update_dst_ptr(&bloom_key, dst_host_rec, record, tmpl_in,
            dir_flags);
      }
   }
   
   fht_unlock_data(dst_lock);
      
   #undef ADD
   #undef INC
}

/** \brief Remove the record by the key
 * Remove the record from hosts stats table. 
 * \param key Key to remove from the table
 */
void HostProfile::remove_by_key(const hosts_key_t &key)
{
   int8_t *lock = NULL;
   hosts_record_t *rec = (hosts_record_t *) fht_get_data_with_stash_locked(
      stat_table, (char*) key.bytes, &lock);
   if (rec == NULL) {
      // not found
      return;
   }
   
   for(sp_list_iter it = sp_list.begin(); it != sp_list.end(); ++it) {
      it->pointers.delete_ptr(*rec);
   }

   int ret = fht_remove_with_stash_locked(stat_table, (char*) key.bytes, lock);
   if (ret) {
      log(LOG_DEBUG, "Failed to remove locked item in remove_by_key.");
      fht_unlock_data(lock);
   }
}

/** \brief Remove subprofiles and clean hosts stats table
 */
void HostProfile::release()
{
   // Remove all subprofiles
   fht_iter_t *table_iter = fht_init_iter(stat_table);
   if (table_iter == NULL) {
      log(LOG_ERR, "Error: Failed to remove all subprofiles. Table iterator failed.");
   } else {
      // Iterate over the table of stats
      while (fht_get_next_iter(table_iter) != FHT_ITER_RET_END) {
         hosts_record_t &temp = *((hosts_record_t *) table_iter->data_ptr);
         
         // Iterate over subprofiles
         for (sp_list_iter it = sp_list.begin(); it != sp_list.end(); ++it) {
            it->pointers.delete_ptr(temp);
         }
      }
      
      fht_destroy_iter(table_iter);
   }
   
   // Clear table
   fht_clear(stat_table);
}

/** \brief Swap/clean BloomFilters
 * Clear active BloomFilter and swap active and learning BloomFilter
 */
void HostProfile::swap_bf()
{
   bf_active->clear();

   bloom_filter *tmp = bf_active;
   bf_active = bf_learn;
   bf_learn = tmp;

   // Swap BloomFilters in subprofiles
   for(sp_list_iter it = sp_list.begin(); it != sp_list.end(); ++it) {
      if (it->pointers.bf_config_ptr == NULL) {
         continue;
      }

      it->pointers.bf_config_ptr(BF_SWAP, 0);
   }
}

/**
 * \brief Check whether there are any incidents in the record
 * \param key Key of the record
 * \param record Record to check
 * \param subprofiles When True check active subprofiles with active detector
 */
void HostProfile::check_record(const hosts_key_t &key, const hosts_record_t &record, 
   bool subprofiles)
{
   // detector
   if (detector_status) {
      check_new_rules(key, record);
   }

   if (!subprofiles) {
      return;
   }

   // call detectors of subprofiles
   for(sp_list_iter it = sp_list.begin(); it != sp_list.end(); ++it) {
      if (!it->detector_status || !it->sp_status) {
         break;
      }

      it->pointers.check_ptr(key, record);
   }
}

/** \brief Get a reference to a record from the table
 * 
 * Find the record in the table. If the record does not exist, create new empty
 * one. Sometime when the empty record is saved, another record is kicked off
 * and sent to the detector.
 *
 * \param[in] key Key of the record
 * \param[out] lock Lock of the record
 * \return Referece to the record
 */
hosts_record_t& HostProfile::get_record(const hosts_key_t& key, int8_t **lock)
{
   hosts_record_t *rec = NULL;
   do {
      rec = (hosts_record_t *) fht_get_data_with_stash_locked(stat_table,
         (char*) key.bytes, lock);

      if (rec == NULL) {
         // the item doesn't exist, create new empty one
         hosts_key_t kicked_key;       // for possibly kicked key
         hosts_record_t kicked_data;   // for possibly kicked data
         hosts_record_t new_empty;

         int rc = fht_insert_with_stash(stat_table, (char*) key.bytes, 
            (void*) &new_empty, (char*) kicked_key.bytes, (void *) &kicked_data);

         switch (rc) {
         case FHT_INSERT_STASH_LOST:
         case FHT_INSERT_LOST:
            // Another item was kicked out of the table
            check_record(kicked_key, kicked_data);

            // Delete subprofiles in the kicked item
            for(sp_list_iter it = sp_list.begin(); it != sp_list.end(); ++it) {
               it->pointers.delete_ptr(kicked_data);
            }
            break;
         case FHT_INSERT_FAILED:
            // Something managed to insert an item sooner -> get record
            break;
         default:
            // Insertion succeeded -> get record
            break;
         }
      }   
   } while (rec == NULL);

   return *rec;
}

/** \breif Check all flow records in table
 * If a record is valid and it is in the table longer than a specified 
 * time or has not been updated for specified time then the record is 
 * checked by detectors and invalidated. 
 * \param[in] check_all If true, ignore (in)active timeout and check every valid flow
 */
void HostProfile::check_table(bool check_all)
{
   // Init table iterator
   fht_iter_t *table_iter = fht_init_iter(stat_table);
   if (table_iter == NULL) {
      log(LOG_ERR, "Error: Failed to check stats table. The table iterator failed.");
      return;
   } 
   
   log(LOG_DEBUG, "Detectors started...");
   uint32_t counter_checked = 0;
   uint32_t counter_intable = 0;
   
   // Iterate over the table of stats
   while (fht_get_next_iter(table_iter) != FHT_ITER_RET_END) {
      ++counter_intable;
      
      // Get record and check timestamps
      hosts_record_t &rec = *((hosts_record_t *) table_iter->data_ptr);
      if (!check_all &&
         rec.first_rec_ts + active_timeout > hs_time &&
         rec.last_rec_ts + inactive_timeout > hs_time) {
         continue;
      }
      
      // Get key and check record
      const hosts_key_t &key = *((hosts_key_t *) table_iter->key_ptr);
      check_record(key, rec);
      
      // Delete subprofiles in the kicked item
      for(sp_list_iter it = sp_list.begin(); it != sp_list.end(); ++it) {
         it->pointers.delete_ptr(rec);
      }
      
      // Remove record
      fht_remove_iter(table_iter);
      
      ++counter_checked;
   }
   
   fht_destroy_iter(table_iter);
   log(LOG_DEBUG, "Detectors ended. Records in the table: %d, "
      "checked and removed: %d", counter_intable, counter_checked);
}
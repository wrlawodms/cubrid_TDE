/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * log_global.c -
 */

#ident "$Id$"

#include "active_tran_server.hpp"
#include "config.h"
#include "file_io.h"
#include "log_append.hpp"
#include "log_archives.hpp"
#include "log_impl.h"
#include "log_storage.hpp"
#include "log_writer.h"
#include "mvcc_table.hpp"
#include "porting.h"
#include "server_type.hpp"
#include "storage_common.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>

/* Variables */
#if !defined(SERVER_MODE)
/* Index onto transaction table for current thread of execution (client) */
int log_Tran_index = -1;
#endif /* !SERVER_MODE */

// *INDENT-OFF*
log_global::log_global ()
  : trantable TRANTABLE_INITIALIZER
  , append ()
  , prior_info ()
  , hdr ()
  , archive ()
  , run_nxchkpt_atpageid (NULL_PAGEID)
#if defined (SERVER_MODE)
  , flushed_lsa_lower_bound (NULL_LSA)
  , chkpt_lsa_lock PTHREAD_MUTEX_INITIALIZER
#endif // SERVER_MODE
  , chkpt_redo_lsa (NULL_LSA)
  , chkpt_every_npages (INT_MAX)
  , rcv_phase (LOG_RECOVERY_ANALYSIS_PHASE)
  , rcv_phase_lsa (NULL_LSA)
#if defined(SERVER_MODE)
  , backup_in_progress (false)
#else // not SERVER_MODE = SA_MODE
  , final_restored_lsa (NULL_LSA)
#endif // not SERVER_MODE = SA_MODE
  , loghdr_pgptr (NULL)
  , flush_info { 0, 0, NULL
#if defined(SERVER_MODE)
                , PTHREAD_MUTEX_INITIALIZER
#endif /* SERVER_MODE */
     }
  , group_commit_info LOG_GROUP_COMMIT_INFO_INITIALIZER
  , writer_info (new logwr_info ())
  , bg_archive_info ()
  , mvcc_table ()
  , unique_stats_table GLOBAL_UNIQUE_STATS_TABLE_INITIALIZER
  , m_ps_lsa_up_to_date (false)
  , m_ps_consensus_flushed_lsa (NULL_LSA)
{
}
// *INDENT-ON*

LOG_GLOBAL log_Gl;

/* Name of the database and logs */
char log_Path[PATH_MAX];
char log_Archive_path[PATH_MAX];
char log_Prefix[PATH_MAX];

const char *log_Db_fullname = NULL;
char log_Name_active[PATH_MAX];
char log_Name_info[PATH_MAX];
char log_Name_bkupinfo[PATH_MAX];
char log_Name_volinfo[PATH_MAX];
char log_Name_bg_archive[PATH_MAX];
char log_Name_removed_archive[PATH_MAX];
char log_Name_metainfo[PATH_MAX];

// *INDENT-OFF*
log_global::~log_global ()
{
  delete writer_info;
}

#if defined (SERVER_MODE)
void
log_global::wakeup_ps_flush_waiters ()
{
  assert (is_active_transaction_server ());
  m_ps_lsa_up_to_date.store (false);
  m_ps_lsa_cv.notify_all ();
}

void
log_global::wait_for_ps_flushed_lsa (const log_lsa &flush_lsa)
{
  std::unique_lock<std::mutex> ulock (m_ps_lsa_mutex);
  while (m_ps_consensus_flushed_lsa < flush_lsa)
    {
      // Only who first notices it's out-dated updates the consensus lsa.
      if (!m_ps_lsa_up_to_date.exchange (true)) 
      {
        const log_lsa consensus_lsa = get_active_tran_server_ptr ()->compute_consensus_lsa ();
        if (consensus_lsa == NULL_LSA)
        {
          continue; // The number of connected nodes is less than the quorum
        }
        assert (m_ps_consensus_flushed_lsa <= consensus_lsa);

        m_ps_consensus_flushed_lsa = consensus_lsa;
      }
      else 
      {
        m_ps_lsa_cv.wait (ulock);
      }
    }
  quorum_consenesus_er_log ("Quorum satisfied: page server flushed LSA = %lld|%d.\n", LSA_AS_ARGS (&log_Gl.m_ps_consensus_flushed_lsa));
}

void
log_global::initialize_log_prior_sender ()
{
  assert (m_prior_sender == nullptr);
  m_prior_sender = std::make_unique<cublog::prior_sender> ();
}

void
log_global::finalize_log_prior_sender ()
{
  assert (m_prior_sender != nullptr);
  m_prior_sender.reset (nullptr);
}

void
log_global::initialize_log_prior_receiver ()
{
  assert (m_prior_recver == nullptr);
  m_prior_recver = std::make_unique<cublog::prior_recver> (prior_info);
}

void
log_global::finalize_log_prior_receiver ()
{
  assert (m_prior_recver != nullptr);
  m_prior_recver.reset (nullptr);
}

cublog::prior_recver &
log_global::get_log_prior_receiver ()
{
  assert (m_prior_recver != nullptr);
  return *m_prior_recver;
}
#endif // SERVER_MODE
// *INDENT-ON*

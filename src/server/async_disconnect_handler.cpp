/*
  *
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

#include "async_disconnect_handler.hpp"
#include "page_server.hpp"
#include "tran_server.hpp"

#include <cassert>
#include <chrono>

template class async_disconnect_handler<page_server::connection_handler>;
template class async_disconnect_handler<tran_server::connection_handler>;

template <typename T_CONN_HANDLER>
async_disconnect_handler<T_CONN_HANDLER>::async_disconnect_handler ()
  : m_terminate { false }
{
  m_thread = std::thread (&async_disconnect_handler::disconnect_loop, std::ref (*this));
}

template <typename T_CONN_HANDLER>
async_disconnect_handler<T_CONN_HANDLER>::~async_disconnect_handler ()
{
  // Call terminate() before the resources that handlers in this can aceess is released. For example, page_server::connection_handler accesses page_server::m_responder, which is released during shutdown.
  assert (m_terminate.load ());
}

template <typename T_CONN_HANDLER>
void
async_disconnect_handler<T_CONN_HANDLER>::disconnect (connection_handler_uptr_t &&handler)
{
  if (!m_terminate.load ())
    {
      std::unique_lock<std::mutex> ulock { m_queue_mtx };
      m_disconnect_queue.emplace (std::move (handler));
      ulock.unlock ();
      m_queue_cv.notify_one ();
    }
  else
    {
      // cannot ask for disconnect after termination
      assert (false);
    }
}

template <typename T_CONN_HANDLER>
void
async_disconnect_handler<T_CONN_HANDLER>::terminate ()
{
  m_terminate.store (true);
  m_queue_cv.notify_one ();

  if ( m_thread.joinable ())
    {
      m_thread.join ();
    }
  else
    {
      assert (m_terminate.load ());
    }

  assert (m_disconnect_queue.empty ());
}

template <typename T_CONN_HANDLER>
void
async_disconnect_handler<T_CONN_HANDLER>::disconnect_loop ()
{
  constexpr std::chrono::seconds one_second { 1 };

  std::queue<connection_handler_uptr_t> disconnect_work_buffer;
  while (!m_terminate.load ())
    {
      {
	std::unique_lock<std::mutex> ulock { m_queue_mtx };
	if (!m_queue_cv.wait_for (ulock, one_second,
				  [this] { return !m_disconnect_queue.empty () || m_terminate.load (); }))
	  {
	    continue;
	  }

	m_disconnect_queue.swap (disconnect_work_buffer);
      }

      while (!disconnect_work_buffer.empty ())
	{
	  connection_handler_uptr_t &front = disconnect_work_buffer.front ();
	  front.reset (nullptr);
	  disconnect_work_buffer.pop ();
	}
    }
}

template <typename T_CONN_HANDLER>
bool
async_disconnect_handler<T_CONN_HANDLER>::is_terminated ()
{
  return m_terminate.load();
}

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

#include "tran_server.hpp"

#include "communication_server_channel.hpp"
#include "disk_manager.h"
#include "error_manager.h"
#include "memory_alloc.h"
#include "server_type.hpp"
#include "system_parameter.h"

#include <cassert>
#include <cstring>
#include <functional>
#include <string>
#include <shared_mutex>

static void assert_is_tran_server ();

tran_server::~tran_server ()
{
  assert (is_transaction_server () || m_page_server_conn_vec.empty ());
  if (is_transaction_server () && !m_page_server_conn_vec.empty ())
    {
      disconnect_all_page_servers ();
    }
}

int
tran_server::register_connection_handler (const std::string &host)
{
  std::string m_ps_hostname;
  auto col_pos = host.find (":");
  int32_t port = -1;

  if (col_pos < 1 || col_pos >= host.length () - 1)
    {
      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HOST_PORT_PARAMETER, 2, prm_get_name (PRM_ID_PAGE_SERVER_HOSTS),
	      host.c_str ());
      return ER_HOST_PORT_PARAMETER;
    }

  try
    {
      port = std::stoi (host.substr (col_pos + 1));
    }
  catch (...)
    {
    }

  if (port < 1 || port > USHRT_MAX)
    {
      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HOST_PORT_PARAMETER, 2, prm_get_name (PRM_ID_PAGE_SERVER_HOSTS),
	      host.c_str ());
      return ER_HOST_PORT_PARAMETER;
    }
  // host and port seem to be OK
  m_ps_hostname = host.substr (0, col_pos);
  er_log_debug (ARG_FILE_LINE, "Page server hosts: %s port: %d\n", m_ps_hostname.c_str (), port);

  m_page_server_conn_vec.emplace_back (create_connection_handler (*this, {port, m_ps_hostname}));

  return NO_ERROR;
}

int
tran_server::register_connection_handlers (std::string &hosts)
{
  auto col_pos = hosts.find (":");

  if (col_pos < 1 || col_pos >= hosts.length () - 1)
    {
      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HOST_PORT_PARAMETER, 2, prm_get_name (PRM_ID_PAGE_SERVER_HOSTS),
	      hosts.c_str ());
      return ER_HOST_PORT_PARAMETER;
    }

  size_t pos = 0;
  std::string delimiter = ",";
  int exit_code = NO_ERROR;

  while ((pos = hosts.find (delimiter)) != std::string::npos)
    {
      std::string token = hosts.substr (0, pos);
      hosts.erase (0, pos + delimiter.length ());

      if (register_connection_handler (token) != NO_ERROR)
	{
	  exit_code = ER_HOST_PORT_PARAMETER;
	}
    }
  if (register_connection_handler (hosts) != NO_ERROR)
    {
      exit_code = ER_HOST_PORT_PARAMETER;
    }

  return exit_code;
}

int
tran_server::boot (const char *db_name)
{
  m_server_name = db_name;

  int error_code = init_page_server_hosts ();
  if (error_code != NO_ERROR)
    {
      ASSERT_ERROR ();
      return error_code;
    }

  error_code = prepare_connections ();
  if (error_code != NO_ERROR)
    {
      ASSERT_ERROR ();
      return error_code;
    }

  /*
    * At least one PS is given by the configuration.
    * Even if uses_remote_storage () == false, the remote storage can exist.
    */
  if (m_page_server_conn_vec.empty () == false)
    {
      const auto start_time = std::chrono::steady_clock::now ();

      while (true)
	{
	  error_code = reset_main_connection ();
	  if (error_code == NO_ERROR)
	    {
	      break;
	    }
	  else
	    {
	      /* TEMPORARY waiting and timeout
	      *
	      * TODO: Remove this and just make sure reset_main_connection doesn't fail
	      *       when the ATS recovery with handshakes with multiple page servers comes in.
	      *
	      * For now, the main connection may not be able to be set for a while. It can be after one it set to CONNECTED state.
	      * When the handshakes comes in, it's guaranteed that at least one connection is completely CONNECTED here
	      * and it will be the main connection. Until then, we just wait here until a conenction is ready.
	      */
	      const auto current_time = std::chrono::steady_clock::now ();
	      const auto duration = std::chrono::duration_cast<std::chrono::seconds> (current_time - start_time).count();
	      if (duration > 30) // timeout: 30 seconds
		{
		  assert (false);
		  return error_code;
		}
	      else
		{
		  constexpr auto sleep_time = std::chrono::milliseconds (30);
		  std::this_thread::sleep_for (sleep_time);
		}
	    }
	}

      m_ps_connector.start ();
    }

  if (uses_remote_storage ())
    {
      error_code = get_boot_info_from_page_server ();
      if (error_code != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  return error_code;
	}
    }

  return NO_ERROR;
}

void
tran_server::push_request (tran_to_page_request reqid, std::string &&payload)
{
  int err_code = NO_ERROR;
  auto slock = std::shared_lock<std::shared_mutex> { m_main_conn_mtx };
  while (true)
    {
      err_code = m_main_conn->push_request (reqid, std::string (payload));
      if (err_code != NO_ERROR && !m_main_conn->is_connected ())
	{
	  // error and the connection is dead.
	  slock.unlock (); // it will be locked exclusively inside reset_main_connection()
	  err_code = reset_main_connection ();
	  if (err_code == ER_CONN_NO_PAGE_SERVER_AVAILABLE)
	    {
	      break; // Nothing can be done. Just ignore for now. TODO
	    }
	  slock.lock ();
	}
      else
	{
	  break;
	}
    }
}

int
tran_server::send_receive (tran_to_page_request reqid, std::string &&payload_in, std::string &payload_out)
{
  int err_code = NO_ERROR;
  auto slock = std::shared_lock<std::shared_mutex> { m_main_conn_mtx };
  while (true)
    {
      err_code = m_main_conn->send_receive (reqid, std::string (payload_in), payload_out);
      if (err_code != NO_ERROR && !m_main_conn->is_connected ())
	{
	  // error and the connection is dead.
	  slock.unlock (); // it will be locked exclusively inside reset_main_connection()
	  err_code = reset_main_connection ();
	  if (err_code == ER_CONN_NO_PAGE_SERVER_AVAILABLE)
	    {
	      return err_code;
	    }
	  slock.lock ();
	}
      else
	{
	  break;
	}
    }

  return err_code;
}

int
tran_server::init_page_server_hosts ()
{
  assert_is_tran_server ();
  assert (m_page_server_conn_vec.empty ());
  /*
   * Specified behavior:
   * ===============================================================================
   * |       \    hosts config     |   empty   |    bad    |          good         |
   * |--------\--------------------|-----------|-----------|------------|----------|
   * | storage \ connections to PS |           |           |    == 0    |   > 0    |
   * |==========\==============================|===========|============|==========|
   * |   local  |                      OK      |    N/A    |     OK     |   OK     |
   * |----------|------------------------------|-----------|------------|----------|
   * |   remote |                     Error    |   Error   |   Error    |   OK     |
   * ===============================================================================
   */

  // read raw config
  //
  std::string hosts = prm_get_string_value (PRM_ID_PAGE_SERVER_HOSTS);
  bool uses_remote_storage = get_remote_storage_config ();

  // check config validity
  //
  if (!hosts.length ())
    {
      if (uses_remote_storage)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_EMPTY_PAGE_SERVER_HOSTS_CONFIG, 0);
	  return ER_EMPTY_PAGE_SERVER_HOSTS_CONFIG;
	}
      else
	{
	  // no page server, local storage
	  assert (is_active_transaction_server ());
	  return NO_ERROR;
	}
    }

  int exit_code = register_connection_handlers (hosts);
  if (m_page_server_conn_vec.empty ())
    {
      // no valid hosts
      int exit_code = ER_HOST_PORT_PARAMETER;
      ASSERT_ERROR_AND_SET (exit_code); // er_set was called
      return exit_code;
    }
  if (exit_code != NO_ERROR)
    {
      //there is at least one correct host in the list
      //clear the errors from parsing the bad ones
      er_clear ();
    }
  exit_code = NO_ERROR;
  // use config to connect
  //
  int valid_connection_count = 0;
  bool failed_conn = false;
  for (const auto &conn : m_page_server_conn_vec)
    {
      exit_code = conn->connect ();
      if (exit_code == NO_ERROR)
	{
	  ++valid_connection_count;
	}
      else
	{
	  failed_conn = true;
	}
    }

  if (failed_conn && valid_connection_count > 0)
    {
      //at least one valid host exists clear the error remaining from previous failing ones
      er_clear ();
      exit_code = NO_ERROR;
    }

  // validate connections vs. config
  //
  if (valid_connection_count == 0 && uses_remote_storage)
    {
      assert (exit_code != NO_ERROR);
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_NO_PAGE_SERVER_CONNECTION, 0);
      exit_code = ER_NO_PAGE_SERVER_CONNECTION;
    }
  else if (valid_connection_count == 0)
    {
      // failed to connect to any page server
      assert (exit_code != NO_ERROR);
      er_clear ();
      exit_code = NO_ERROR;
    }
  er_log_debug (ARG_FILE_LINE, "Transaction server runs on %s storage.",
		uses_remote_storage ? "remote" : "local");
  return exit_code;
}

/* NOTE : Since TS don't need the information about the number of permanent volume during boot,
 *        this message has no actual use currently. However, this mechanism will be reserved,
 *        because it can be used in the future when multiple PS's are supported. */
int
tran_server::get_boot_info_from_page_server ()
{
  std::string response_message;
  const int error_code = send_receive (tran_to_page_request::GET_BOOT_INFO, std::string (), response_message);
  if (error_code != NO_ERROR)
    {
      ASSERT_ERROR ();
      return error_code;
    }

  DKNVOLS nvols_perm;
  std::memcpy (&nvols_perm, response_message.c_str (), sizeof (nvols_perm));

  /* Check the dummay value whether the TS receives the message from PS (receive_boot_info_request) well. */
  assert (nvols_perm == VOLID_MAX);

  return NO_ERROR;
}

int
tran_server::connection_handler::connect ()
{
  auto ps_conn_error_lambda = [this] (const std::lock_guard<std::shared_mutex> &)
  {
    m_state = state::IDLE;

    er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_NET_PAGESERVER_CONNECTION, 1, m_node.get_host ().c_str ());
    return ER_NET_PAGESERVER_CONNECTION;
  };

  assert_is_tran_server ();

  {
    auto lockg_state = std::lock_guard<std::shared_mutex> { m_state_mtx };
    assert (m_state == state::IDLE);

    m_state = state::CONNECTING;

    // connect to page server
    constexpr int CHANNEL_POLL_TIMEOUT = 1000;    // 1000 milliseconds = 1 second
    cubcomm::server_channel srv_chn (m_ts.m_server_name.c_str (), SERVER_TYPE_PAGE, CHANNEL_POLL_TIMEOUT);

    srv_chn.set_channel_name ("TS_PS_comm");

    css_error_code comm_error_code = srv_chn.connect (m_node.get_host ().c_str (), m_node.get_port (),
				     CMD_SERVER_SERVER_CONNECT);
    if (comm_error_code != css_error_code::NO_ERRORS)
      {
	return ps_conn_error_lambda (lockg_state);
      }

    if (srv_chn.send_int (static_cast<int> (m_ts.m_conn_type)) != NO_ERRORS)
      {
	return ps_conn_error_lambda (lockg_state);
      }

    int returned_code;
    if (srv_chn.recv_int (returned_code) != css_error_code::NO_ERRORS)
      {
	return ps_conn_error_lambda (lockg_state);
      }
    if (returned_code != static_cast<int> (m_ts.m_conn_type))
      {
	return ps_conn_error_lambda (lockg_state);
      }

    set_connection (std::move (srv_chn));

    er_log_debug (ARG_FILE_LINE, "Transaction server successfully connected to the page server. Channel id: %s.\n",
		  srv_chn.get_channel_id ().c_str ());
  }

  // Do the preliminary jobs depending on the server type before opening the connection to the outside.
  // the state will be transitioned to CONNECTED by transition_to_connected().
  transition_to_connected ();

  return NO_ERROR;
}

void
tran_server::disconnect_all_page_servers ()
{
  assert_is_tran_server ();

  m_ps_connector.terminate ();

  for (auto &conn : m_page_server_conn_vec)
    {
      constexpr bool with_disconnect_msg = true;
      conn->disconnect_async (with_disconnect_msg);
    }

  for (auto &conn : m_page_server_conn_vec)
    {
      conn->wait_async_disconnection ();
    }

  er_log_debug (ARG_FILE_LINE, "Transaction server disconnected from all page servers.");
}

int
tran_server::reset_main_connection ()
{
  auto ulock = std::unique_lock<std::shared_mutex> { m_main_conn_mtx };

  /* the priority to select the main connection is the order in the container */
  const auto main_conn_cand_it = std::find_if (m_page_server_conn_vec.cbegin (), m_page_server_conn_vec.cend (),
				 [] (const auto &conn)
  {
    return conn->is_connected ();
  });

  if (main_conn_cand_it == m_page_server_conn_vec.cend ())
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CONN_NO_PAGE_SERVER_AVAILABLE, 0);
      return ER_CONN_NO_PAGE_SERVER_AVAILABLE;
    }

  if (m_main_conn != main_conn_cand_it->get ())
    {
      m_main_conn = main_conn_cand_it->get ();
      er_log_debug (ARG_FILE_LINE, "The main connection is set to %s.\n",
		    m_main_conn->get_channel_id ().c_str ());
    }

  return NO_ERROR;
}

bool
tran_server::is_page_server_connected () const
{
  assert_is_tran_server ();
  return std::any_of (m_page_server_conn_vec.cbegin (), m_page_server_conn_vec.cend (),	[] (const auto &conn)
  {
    return conn->is_connected ();
  });
}

bool
tran_server::uses_remote_storage () const
{
  return false;
}

bool tran_server::get_main_connection_info (std::string &host_out, int32_t &port_out)
{
  auto slock = std::shared_lock<std::shared_mutex> { m_main_conn_mtx };
  if (m_main_conn == nullptr)
    {
      host_out = "N/A";
      port_out = -1;
      return false;
    }

  auto &node = m_main_conn->get_node ();
  host_out = node.get_host ();
  port_out = node.get_port ();

  return true;
}

void
tran_server::connection_handler::set_connection (cubcomm::channel &&chn)
{
  constexpr size_t RESPONSE_PARTITIONING_SIZE = 24;   // Arbitrarily chosen
  // TODO: to reduce contention as much as possible, should be equal to the maximum number
  // of active transactions that the system allows (PRM_ID_CSS_MAX_CLIENTS) + 1

  auto send_error_handler = std::bind (&tran_server::connection_handler::send_error_handler, this,
				       std::placeholders::_1, std::placeholders::_2);
  auto recv_error_handler = std::bind (&tran_server::connection_handler::recv_error_handler, this,
				       std::placeholders::_1);

  auto lockg_conn = std::lock_guard<std::shared_mutex> { m_conn_mtx };

  assert (m_conn == nullptr);
  m_conn.reset (new page_server_conn_t (std::move (chn), get_request_handlers (), tran_to_page_request::RESPOND,
					page_to_tran_request::RESPOND, RESPONSE_PARTITIONING_SIZE, std::move (send_error_handler),
					std::move (recv_error_handler)));

  m_conn->start ();
}

void
tran_server::connection_handler::send_error_handler (css_error_code error_code, bool &abort_further_processing)
{
  abort_further_processing = false;

  // Remove the connection_handler if the internal socket is closed. It's been disconnected abnormally.
  if (error_code == CONNECTION_CLOSED)
    {
      abort_further_processing = true;
      er_log_debug (ARG_FILE_LINE,
		    "send_error_handler: an abnormal disconnection has been detected. channel id: %s.\n", get_channel_id ().c_str ());

      constexpr auto with_disc_msg = false;
      disconnect_async (with_disc_msg);
    }
  else
    {
      er_log_debug (ARG_FILE_LINE, "send_error_handler: error code: %d, channel id: %s.\n", error_code,
		    get_channel_id ().c_str ());
    }
}

void
tran_server::connection_handler::recv_error_handler (css_error_code error_code)
{
  constexpr auto with_disc_msg = false;
  disconnect_async (with_disc_msg);
  er_log_debug (ARG_FILE_LINE,
		"recv_error_handler: an abnormal disconnection has been detected. channel id: %s.\n", get_channel_id ().c_str ());
}

tran_server::connection_handler::~connection_handler ()
{
  wait_async_disconnection (); // join the async disconneciton job if exists
}

void
tran_server::connection_handler::disconnect_async (bool with_disc_msg)
{
  auto lockg = std::lock_guard <std::shared_mutex> { m_state_mtx };

  if (m_state == state::IDLE || m_state == state::DISCONNECTING)
    {
      return; // already disconnected by other
    }

  assert (m_state == state::CONNECTING || m_state == state::CONNECTED);
  m_state = state::DISCONNECTING;

  m_disconn_future = std::async (std::launch::async, [this, with_disc_msg]
  {
    on_disconnecting (); // server-type specific jobs before disconnecting.

    /*
     * Stop incoming communication and wake up threads waiting for a response, informing them that it won't be served.
     * - If the function `disconnect_async` is called more than once for a connection handler,
     *   it will be guarded by the state checks at the beginning of the function (see above).
     *   This can occur due to SEND_DISCONNECT_REQUEST_MSG and error handlers.
     * - This must be outside the lock of m_conn_mtx because waiters are holding the lock during send_receive().
     * - m_conn is not nullptr here since it's set to nullptr only below this point.
     */
    m_conn->stop_incoming_communication_thread ();

    auto lockg_state = std::lock_guard { m_state_mtx };
    auto lockg_conn = std::lock_guard { m_conn_mtx };
    const std::string channel_id = get_channel_id ();

    assert (m_state == state::DISCONNECTING);

    if (with_disc_msg)
      {
	const int payload = static_cast<int> (m_ts.m_conn_type);
	std::string msg (reinterpret_cast<const char *> (&payload), sizeof (payload));
	m_conn->push (tran_to_page_request::SEND_DISCONNECT_MSG, std::move (msg));
	// After sending SEND_DISCONNECT_MSG, the page server may release all resources related to this connection.
	// So, it has to be the last msg.
      }

    m_conn.reset (nullptr);
    er_log_debug (ARG_FILE_LINE, "Transaction server has been disconnected from the page server with channel id: %s.\n", channel_id.c_str ());

    m_state = state::IDLE;
  });
}

void
tran_server::connection_handler::wait_async_disconnection ()
{
  if (m_disconn_future.valid ())
    {
      m_disconn_future.get ();
    }
#if !defined (NDEBUG)
  auto slock = std::shared_lock<std::shared_mutex> { m_state_mtx };
  assert (m_state == state::IDLE);
#endif /* !NDEBUG */
}

tran_server::connection_handler::request_handlers_map_t
tran_server::connection_handler::get_request_handlers ()
{
  // Insert handlers specific to all transaction servers here.
  // For now, there are no such handlers; return an empty map.
  std::map<page_to_tran_request, page_server_conn_t::incoming_request_handler_t> handlers_map;

  auto disconnect_request_handler = std::bind (&tran_server::connection_handler::receive_disconnect_request, this,
				    std::placeholders::_1);

  handlers_map.insert (std::make_pair (page_to_tran_request::SEND_DISCONNECT_REQUEST_MSG, disconnect_request_handler));

  return handlers_map;
}

void
tran_server::connection_handler::receive_disconnect_request (page_server_conn_t::sequenced_payload &&)
{
  constexpr bool with_disconnect_msg = true;
  disconnect_async (with_disconnect_msg);
}

int
tran_server::connection_handler::push_request (tran_to_page_request reqid, std::string &&payload)
{
  auto slock_state = std::shared_lock<std::shared_mutex> { m_state_mtx };

  if (m_state != state::CONNECTED)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CONN_PAGE_SERVER_CANNOT_BE_REACHED, 0);
      return ER_CONN_PAGE_SERVER_CANNOT_BE_REACHED;
    }

  // state::CONNECTED guarantees that the internal m_node is not nullptr.
  auto slock_conn = std::shared_lock<std::shared_mutex> { m_conn_mtx };
  slock_state.unlock ();

  m_conn->push (reqid, std::move (payload));

  return NO_ERROR;
}

void
tran_server::connection_handler::push_request_regardless_of_state (tran_to_page_request reqid, std::string &&payload)
{
  auto slock_conn = std::shared_lock<std::shared_mutex> { m_conn_mtx };
  m_conn->push (reqid, std::move (payload));
}

int
tran_server::connection_handler::send_receive (tran_to_page_request reqid, std::string &&payload_in,
    std::string &payload_out)
{
  auto slock_state = std::shared_lock<std::shared_mutex> { m_state_mtx };

  if (m_state != state::CONNECTED)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CONN_PAGE_SERVER_CANNOT_BE_REACHED, 0);
      return ER_CONN_PAGE_SERVER_CANNOT_BE_REACHED;
    }

  // state::CONNECTED guarantees that the internal m_node is not nullptr.
  auto slock_conn = std::shared_lock<std::shared_mutex> { m_conn_mtx };
  slock_state.unlock (); // to allow to disconnect while waiting for its reply

  const css_error_code error_code = m_conn->send_recv (reqid, std::move (payload_in), payload_out);
  if (error_code != NO_ERRORS)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CONN_PAGE_SERVER_CANNOT_BE_REACHED, 0);
      return ER_CONN_PAGE_SERVER_CANNOT_BE_REACHED;
    }

  return NO_ERROR;
}

const std::string
tran_server::connection_handler::get_channel_id () const
{
  // Make sure that m_conn is not nullptr
  return m_conn->get_underlying_channel_id ();
}

bool
tran_server::connection_handler::is_connected ()
{
  auto slock = std::shared_lock<std::shared_mutex> { m_state_mtx };
  return m_state == state::CONNECTED;
}

bool
tran_server::connection_handler::is_idle ()
{
  auto slock = std::shared_lock<std::shared_mutex> { m_state_mtx };
  return m_state == state::IDLE;
}

const cubcomm::node &
tran_server::connection_handler::get_node () const
{
  return m_node;
}

tran_server::ps_connector::ps_connector (tran_server &ts)
  : m_ts { ts }
  , m_daemon { nullptr }
  , m_terminate { true }
{
}

tran_server::ps_connector::~ps_connector ()
{
  terminate ();
}

void
tran_server::ps_connector::start ()
{
  assert (m_terminate.load () == true);
  /* After init_page_server_hosts() */
  assert (m_ts.m_page_server_conn_vec.empty () == false);

  auto func_exec = std::bind (&tran_server::ps_connector::try_connect_to_all_ps, std::ref (*this), std::placeholders::_1);
  auto entry = new cubthread::entry_callable_task (std::move (func_exec));

  m_terminate.store (false);

  constexpr std::chrono::seconds five_sec { 5 };
  cubthread::looper loop (five_sec);
  m_daemon = cubthread::get_manager ()->create_daemon (loop, entry, "tran_server::ps_connector");
  assert (m_daemon != NULL);
}

void
tran_server::ps_connector::terminate ()
{
  if (m_terminate.exchange (true) == false)
    {
      cubthread::get_manager ()->destroy_daemon (m_daemon);
    }
}

void
tran_server::ps_connector::try_connect_to_all_ps (cubthread::entry &)
{
  bool newly_connected = false;
  for (const auto &conn : m_ts.m_page_server_conn_vec)
    {
      if (conn->is_idle ())
	{
	  /*
	   * TODO It can be too verbose now since it always complain to fail to connect when a PS has been stopped.
	   * Later on, this job is going to be triggered by a cluster manager or cub_master when a PS is ready to connect.
	   */
	  if (conn->connect () == NO_ERROR)
	    {
	      newly_connected = true;
	    }
	}

      if (m_terminate.load ())
	{
	  return;
	}
    }

  if (newly_connected)
    {
      // It should be done when the connection_handler's state gets CONNECTED.
      // TODO in near future, it will be CONNECTING right after connect() and become CONNECTED asynchronously, then it should be changed along.
      if (m_ts.reset_main_connection () != NO_ERROR)
	{
	  assert (false);
	}
    }
}

void
assert_is_tran_server ()
{
  assert (get_server_type () == SERVER_TYPE::SERVER_TYPE_TRANSACTION);
}

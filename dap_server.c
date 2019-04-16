/*
 Copyright (c) 2017-2018 (c) Project "DeM Labs Inc" https://github.com/demlabsinc
  All rights reserved.

 This file is part of DAP (Deus Applications Prototypes) the open source project

    DAP (Deus Applicaions Prototypes) is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    DAP is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with any DAP based project.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>

#define NI_NUMERICHOST  1 /* Don't try to look up hostname.  */
#define NI_NUMERICSERV  2 /* Don't convert port number to name.  */
#define NI_NOFQDN       4 /* Only return nodename portion.  */
#define NI_NAMEREQD     8 /* Don't return numeric addresses.  */
#define NI_DGRAM       16 /* Look up UDP service rather than TCP.  */

#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>

#include "dap_common.h"
#include "dap_server.h"
//#include <ev.h>

#define LOG_TAG "server"

#define DAP_MAX_THREAD_EVENTS   				8192
#define DAP_MAX_THREADS         				16

#define	SOCKET_TIMEOUT_TIME							60
#define	SOCKETS_TIMEOUT_CHECK_PERIOD		5

static  int _count_threads = 0;
static  int epoll_max_events = 0;
static  bool  bQuitSignal = false;

static  struct epoll_event  **thread_events = NULL;
static  int g_efd[ DAP_MAX_THREADS ];
static  dap_server_t *_current_run_server = NULL;
static  void read_write_cb( dap_client_remote_t *dap_cur, int revents );

static  struct thread_information {
    int thread_number;
    atomic_size_t count_open_connections;
} *thread_inform = NULL;

/*
===============================================
  get_epoll_max_user_watches( )

  return max epoll() event watches
===============================================
*/
static int  get_epoll_max_user_watches( void )
{
  int   v = 0, len = 0;
  char  str[32];

  FILE *fp = fopen("/proc/sys/fs/epoll/max_user_watches", "r" );
  if ( !fp ) {
    printf("can't open proc/sys/fs/epoll/max_user_watches\n");
    return v;
  }

  len = fread( &str[0], 1, 31, fp );
  if ( !len ) {
    return v;
  }

  str[ len ] = 0;
  v = atoi( str );

  return v;
}

/*
===============================================
  dap_server_init( )

  Init server module
  return Zero if ok others if no
===============================================
*/
int dap_server_init( int count_threads )
{
  _count_threads = count_threads;
  //  _count_threads = 1;

  log_it( L_DEBUG, "dap_server_init() threads %u", count_threads );

  signal( SIGPIPE, SIG_IGN );

  thread_inform = (struct thread_information *)malloc( sizeof(struct thread_information) * _count_threads );
  if ( !thread_inform )
    goto err;

  thread_events = (struct epoll_event **)malloc( sizeof(struct epoll_event *) * _count_threads );
  if ( !thread_events )
    goto err;

  memset( thread_events, 0, sizeof(struct epoll_event *) * _count_threads );

  epoll_max_events = get_epoll_max_user_watches( );
  if ( epoll_max_events > DAP_MAX_THREAD_EVENTS )
    epoll_max_events = DAP_MAX_THREAD_EVENTS;

  for( int i = 0; i < _count_threads; ++ i ) {

    g_efd[ i ] = -1;
    atomic_init( &thread_inform[i].count_open_connections, 0 );

    thread_inform[ i ].thread_number = i;
    thread_events[ i ] = (struct epoll_event *)malloc( sizeof(struct epoll_event) * epoll_max_events );

    if ( !thread_events[i] )
      goto err;
  }

  log_it( L_NOTICE, "Initialized socket server module" );

  dap_client_remote_init( );

  return 0;

err:;

  dap_server_deinit( );

  return 1;
}

/*
=========================================================
  dap_server_deinit( )

  Deinit server module
=========================================================
*/
void  dap_server_deinit( void )
{
  dap_client_remote_deinit( );

  if ( thread_events ) {
    for( int i = 0; i < _count_threads; ++ i ) {
      if ( thread_events[i] )
        free( thread_events[i] );
    }
    free( thread_events );
  }

  if ( thread_inform )
    free( thread_inform );
}

/*
=========================================================
  dap_server_new( )

  Creates new empty instance of dap_server_t
=========================================================
*/
dap_server_t  *dap_server_new( void )
{
  return (dap_server_t *)calloc( 1, sizeof(dap_server_t) );
}

/*
=========================================================
  dap_server_new( )

  Delete server instance
=========================================================
*/
void dap_server_delete( dap_server_t *sh )
{
  dap_client_remote_t *dap_cur, *tmp;

  if ( !sh ) return;

  if( sh->address )
    free( sh->address );

  HASH_ITER( hh, sh->clients, dap_cur, tmp )
    dap_client_remote_remove( dap_cur, sh );

  if( sh->server_delete_callback )
    sh->server_delete_callback( sh, NULL );

  if ( sh->_inheritor )
    free( sh->_inheritor );

  free( sh );
}

/*
=========================================================
  set_nonblock_socket( )
=========================================================
*/
int set_nonblock_socket( int fd )
{
  int flags;

  flags = fcntl( fd, F_GETFL );
  flags |= O_NONBLOCK;

  return fcntl( fd, F_SETFL, flags );
}

/*
=========================================================
  read_write_cb( )

=========================================================
*/
static void read_write_cb( dap_client_remote_t *dap_cur, int revents )
{
//  log_it( L_NOTICE, "[THREAD %u] read_write_cb fd %u revents %u", dap_cur->tn, dap_cur->socket, revents );
//  sleep( 5 ); // ?????????

  if( !dap_cur ) {

    log_it( L_ERROR, "read_write_cb: dap_client_remote NULL" );
    return;
  }

  if ( revents & EPOLLIN ) {

//    log_it( L_DEBUG, "[THREAD %u] socket read %d ", dap_cur->tn, dap_cur->socket );

    int bytes_read = recv( dap_cur->socket,
                                  dap_cur->buf_in + dap_cur->buf_in_size,
                                  sizeof(dap_cur->buf_in) - dap_cur->buf_in_size,
                                  0 );
    if( bytes_read > 0 ) {
//      log_it( L_DEBUG, "[THREAD %u] read %u socket client said: %s", dap_cur->tn, bytes_read, dap_cur->buf_in + dap_cur->buf_in_size );

      dap_cur->buf_in_size += (size_t)bytes_read;
      dap_cur->upload_stat.buf_size_total += (size_t)bytes_read;

      _current_run_server->client_read_callback( dap_cur ,NULL );
    }
    else if ( bytes_read < 0 ) {
      log_it( L_ERROR,"Bytes read Error %s",strerror(errno) );
      if ( strcmp(strerror(errno),"Resource temporarily unavailable") != 0 )
      dap_cur->signal_close = true;
    }
    else { // bytes_read == 0
      dap_cur->signal_close = true;
      log_it( L_DEBUG, "0 bytes read" );
    }
  }

  if( ( (revents & EPOLLOUT) || dap_cur->_ready_to_write ) && dap_cur->signal_close == false ) {

//    log_it(L_DEBUG, "[THREAD %u] socket write %d ", dap_cur->tn, dap_cur->socket );
    _current_run_server->client_write_callback( dap_cur, NULL ); // Call callback to process write event

    if( dap_cur->buf_out_size == 0 ) {
//      log_it(L_DEBUG, "dap_cur->buf_out_size = 0, set ev_read watcher " );

      dap_cur->pevent.events = EPOLLIN | EPOLLERR;
      if( epoll_ctl(dap_cur->efd, EPOLL_CTL_MOD, dap_cur->socket, &dap_cur->pevent) != 0 ) {
        log_it( L_ERROR, "epoll_ctl failed 003" );
      }
    }
    else {
//      log_it(L_DEBUG, "[THREAD %u] send dap_cur->buf_out_size = %u , %s", dap_cur->tn, dap_cur->buf_out_size, dap_cur->buf_out );

      size_t total_sent = dap_cur->buf_out_offset;

      while ( total_sent < dap_cur->buf_out_size ) {
        //log_it(DEBUG, "Output: %u from %u bytes are sent ", total_sent, dap_cur->buf_out_size);
        ssize_t bytes_sent = send( dap_cur->socket,
                                   dap_cur->buf_out + total_sent,
                                   dap_cur->buf_out_size - total_sent,
                                   MSG_DONTWAIT | MSG_NOSIGNAL );
        if( bytes_sent < 0 ) {
          log_it(L_ERROR,"[THREAD %u] Error occured in send() function %s", dap_cur->tn, strerror(errno) );
          break;
        }

        total_sent += (size_t)bytes_sent;
        dap_cur->download_stat.buf_size_total += (size_t)bytes_sent;
      }

      if( total_sent == dap_cur->buf_out_size ) {
        dap_cur->buf_out_offset = dap_cur->buf_out_size  = 0;
//        dap_cur->signal_close = true; // REMOVE ME!!!!!!!!!!!!!!11
      }
      else {
        dap_cur->buf_out_offset = total_sent;
      }
    } // else
  } // write

  if ( dap_cur->signal_close ) {

//    log_it( L_INFO, "[THREAD %u] signal_close: Close Socket %d", dap_cur->tn, dap_cur->socket );

    atomic_fetch_sub( &thread_inform[dap_cur->tn].count_open_connections, 1 );

    if ( epoll_ctl( dap_cur->efd, EPOLL_CTL_DEL, dap_cur->socket, &dap_cur->pevent ) == -1 )
      log_it( L_ERROR,"Can't remove event socket's handler from the epoll_fd" );
    else
      log_it( L_DEBUG,"[THREAD %u] Removed epoll's event fd %u", dap_cur->tn, dap_cur->socket );

    dap_client_remote_remove( dap_cur, _current_run_server );
  }

}

/*
=========================================================
  get_thread_min_connections( )

  return number thread which has minimum open connections
=========================================================
*/
static inline uint8_t get_thread_index_min_connections( )
{
  uint8_t min = 0;

  for( uint32_t i = 1; i < _count_threads; i ++ ) {

    if ( atomic_load(&thread_inform[min].count_open_connections ) >
             atomic_load(&thread_inform[i].count_open_connections) ) {
      min = i;
    }
  }

  return min;
}

/*
=========================================================
  print_online( )

=========================================================
*/
static inline void print_online()
{
  for( uint32_t i = 0; i < _count_threads; i ++ )  {
    log_it(L_INFO, "Thread number: %d, count: %d", 
               thread_inform[i].thread_number, atomic_load(&thread_inform[i].count_open_connections) );
  }
}

/*
=========================================================
  dap_server_listen( )

  Create server_t instance and start to listen tcp port with selected address

=========================================================
*/
dap_server_t *dap_server_listen( const char *addr, uint16_t port, dap_server_type_t type )
{
  dap_server_t* sh = dap_server_new( );

  sh->socket_listener = -111;

  if( type == DAP_SERVER_TCP )
    sh->socket_listener = socket( AF_INET, SOCK_STREAM, 0 );
  else
    return NULL;
  
  if ( set_nonblock_socket(sh->socket_listener) == -1 ) {
    log_it( L_WARNING, "error server socket nonblock" );
    exit( EXIT_FAILURE );
  }

  if ( sh->socket_listener < 0 ) {
    log_it ( L_ERROR,"Socket error %s", strerror(errno) );
    dap_server_delete( sh );
    return NULL;
  }

  log_it( L_NOTICE," Socket created..." );

  int reuse = 1;

  if ( reuse ) 
    if ( setsockopt( sh->socket_listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0 )
      log_it( L_WARNING, "Can't set up REUSEADDR flag to the socket" );

  sh->listener_addr.sin_family = AF_INET;
  sh->listener_addr.sin_port = htons( port );
  inet_pton( AF_INET, addr, &(sh->listener_addr.sin_addr) );

  if( bind(sh->socket_listener, (struct sockaddr *)&(sh->listener_addr), sizeof(sh->listener_addr)) < 0 ) {
    log_it( L_ERROR,"Bind error: %s",strerror(errno) );
    dap_server_delete( sh );
    return NULL;
  }

  log_it( L_INFO,"Binded %s:%u", addr, port );
  listen( sh->socket_listener, DAP_MAX_THREAD_EVENTS * _count_threads );

  log_it( L_INFO,"pthread_mutex_init" );
  pthread_mutex_init( &sh->mutex_on_hash, NULL );

  return sh;
}


static void s_socket_info_all_check_activity( int tn, time_t cur_time )
{
  dap_client_remote_t *dap_cur, *tmp;

  log_it( L_INFO,"s_socket_info_all_check_activity()" );
#if 0
	pthread_mutex_lock( &_current_run_server->mutex_on_hash );
	HASH_ITER( hh, _current_run_server->clients, dap_cur, tmp ) {

		if ( dap_cur->last_time_active + SOCKET_TIMEOUT_TIME >= cur_time ) {

      log_it( L_INFO, "Socket %u timeout, closing...", dap_cur->socket );

	    if ( epoll_ctl( dap_cur->efd, EPOLL_CTL_DEL, dap_cur->socket, &dap_cur->pevent ) == -1 )
      	log_it( L_ERROR,"Can't remove event socket's handler from the epoll_fd" );

			atomic_fetch_sub( &thread_inform[tn].count_open_connections, 1 );
      dap_client_remote_remove( dap_cur, _current_run_server );
		}

	}
	pthread_mutex_unlock( &_current_run_server->mutex_on_hash );
#endif
}


/*
=========================================================
  thread_loop( )

  Server listener thread loop
=========================================================
*/
void  *thread_loop( void *arg )
{
  int tn = *(int *)arg; // thread number
  int efd = g_efd[ tn ];
  struct epoll_event  *events = thread_events[ tn ];
	time_t	next_time_timeout_check = time( NULL ) + SOCKETS_TIMEOUT_CHECK_PERIOD;

  log_it(L_NOTICE, "Start loop listener socket thread %d", tn );

  cpu_set_t mask;
  CPU_ZERO( &mask );
  CPU_SET( tn, &mask );

  if ( pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &mask) != 0 ) {
    log_it( L_CRITICAL, "Error pthread_setaffinity_np() You really have %d or more core in CPU?", tn );
    abort();
  }

  do {

		//log_it( L_DEBUG,"[THREAD %u] EPOLL_WAIT[%u] connections: %u", tn, efd, atomic_load(&thread_inform[tn].count_open_connections) );
    int n = epoll_wait( efd, events, DAP_MAX_THREAD_EVENTS, 1000 );

    if ( n == -1 || bQuitSignal )
      break;

		time_t cur_time = time( NULL );

    for ( int i = 0; i < n; ++ i ) {

      //dap_client_remote_t *dap_cur = dap_client_remote_find( events[i].data.fd, _current_run_server );
      dap_client_remote_t *dap_cur = (dap_client_remote_t *)events[i].data.ptr;

      if ( !dap_cur ) {
        //if ( epoll_ctl( efd, EPOLL_CTL_DEL, events[i].data.fd, &events[i] ) == -1 )
        log_it( L_ERROR,"dap_client_remote_t NULL" );
				continue;
      }

			dap_cur->last_time_active = cur_time;

      if( events[i].events & EPOLLERR ) {
          log_it( L_ERROR,"Socket error: %u, remove it" , dap_cur->socket );
          dap_cur->signal_close = true;
      }

      if ( dap_cur->signal_close ) {
        log_it( L_INFO, "Got signal to close from the client %s", dap_cur->hostaddr );

        if ( epoll_ctl(efd, EPOLL_CTL_DEL, dap_cur->socket, &events[i] ) == -1 )
            log_it( L_ERROR,"Can't remove event socket's handler from the epoll_fd" );

        atomic_fetch_sub( &thread_inform[tn].count_open_connections, 1 );

        dap_client_remote_remove( dap_cur, _current_run_server );
        continue;
      }

      read_write_cb( dap_cur, events[i].events );
    }

		if ( !tn && cur_time >= next_time_timeout_check ) {

			s_socket_info_all_check_activity( tn, cur_time );
			next_time_timeout_check = cur_time + SOCKETS_TIMEOUT_CHECK_PERIOD;
		}

  } while( !bQuitSignal ); 

  return NULL;
}

/*
=========================================================
  dap_server_loop( )

  Main server loop

  @param a_server Server instance
  @return Zero if ok others if not
=========================================================
*/
int dap_server_loop( dap_server_t *d_server )
{
  static int pickthread = 0;  // just for test
  int       thread_arg[ DAP_MAX_THREADS ];
  pthread_t thread_listener[ DAP_MAX_THREADS ];

  if ( !d_server ) return 1;

  for( int i = 0; i < _count_threads; ++i ) {

    int efd = epoll_create1( 0 );
    if ( efd == -1 ) {
      log_it( L_ERROR, "Server wakeup no events / error" );
        goto error;
    }
    g_efd[ i ] = efd;
  }

  for( int i = 0; i < _count_threads; ++i ) {

    thread_arg[ i ] = (int)i;
    pthread_create( &thread_listener[i], NULL, thread_loop, &thread_arg[i] );
  }

  _current_run_server = d_server;

  int efd = epoll_create1( 0 );
	if ( efd == -1 )
		goto error;

  struct epoll_event  pev;
  struct epoll_event  events[ 16 ];

  pev.events = EPOLLIN | EPOLLERR;
  pev.data.fd = d_server->socket_listener;

  if( epoll_ctl( efd, EPOLL_CTL_ADD, d_server->socket_listener, &pev) != 0) {
    log_it( L_ERROR, "epoll_ctl failed 004" );
    return 99;
  }

  while( 1 ) {

    int n = epoll_wait( efd, &events[0], 16, -1 );

    if ( n <= 0 ) {
      log_it( L_ERROR, "Server wakeup no events / error" );
      break;
    }

    for( int i = 0; i < n; ++ i ) {

      if ( events[i].events & EPOLLIN ) {

        int client_fd = accept( events[i].data.fd, 0, 0 );

        if ( client_fd < 0 ) {
          log_it( L_ERROR, "accept_cb: error accept socket");
          continue;
        }

        set_nonblock_socket( client_fd );

          int indx_min = get_thread_index_min_connections( );
//        int indx_min = pickthread & 3; // just for test
//        ++ pickthread; // just for test

        atomic_fetch_add( &thread_inform[indx_min].count_open_connections, 1 );

        log_it( L_DEBUG, "accept %d Client, thread %d", client_fd, indx_min );

				dap_client_remote_t *cr = dap_client_remote_create( _current_run_server, client_fd, indx_min, g_efd[indx_min] );
				cr->time_connection = cr->last_time_active = time( NULL) ;

        if ( epoll_ctl( g_efd[indx_min], EPOLL_CTL_ADD, client_fd, &cr->pevent) != 0) {
          log_it( L_ERROR, "epoll_ctl failed 005" );
          return 99;
        }

      }
      else if( events[i].events & EPOLLERR ) {
        log_it( L_ERROR, "Server socket error event" );
        goto exit;
      }

    } // for

  } // while

exit:;

	bQuitSignal = true;

  close( efd );
  return 0;

error:;

	bQuitSignal = true;

  for( int i = 0; i < _count_threads; ++i ) {
    if ( g_efd[i] != -1 )
      close( g_efd[i] );
  }

  return 0;
}

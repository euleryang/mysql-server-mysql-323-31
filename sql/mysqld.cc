/* Copyright (C) 2000 MySQL AB & MySQL Finland AB & TCX DataKonsult AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "mysql_priv.h"
#include <mysql.h>
#include <m_ctype.h>
#include "sql_acl.h"
#include "slave.h"
#ifdef HAVE_BERKELEY_DB
#include "ha_berkeley.h"
#endif
#ifdef HAVE_INNOBASE_DB
#include "ha_innobase.h"
#endif
#include "ha_myisam.h"
#include <nisam.h>
#include <thr_alarm.h>
#include <ft_global.h>

#ifndef DBUG_OFF
#define ONE_THREAD
#endif

#ifdef	__cplusplus
extern "C" {					// Because of SCO 3.2V4.2
#endif
#include <errno.h>
#include <sys/stat.h>
#ifndef __GNU_LIBRARY__
#define __GNU_LIBRARY__				// Skip warnings in getopt.h
#endif
#include <getopt.h>
#ifdef HAVE_SYSENT_H
#include <sysent.h>
#endif
#ifdef HAVE_PWD_H
#include <pwd.h>				// For getpwent
#endif
#ifdef HAVE_GRP_H
#include <grp.h>
#endif

#ifndef __WIN__
#include <sys/resource.h>
#ifdef HAVE_SYS_UN_H
#  include <sys/un.h>
#endif
#include <netdb.h>
#ifdef HAVE_SELECT_H
#  include <select.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <sys/utsname.h>
#else
#include <windows.h>
#endif // __WIN__

#ifdef HAVE_LIBWRAP
#include <tcpd.h>
#include <syslog.h>
#ifdef NEED_SYS_SYSLOG_H
#include <sys/syslog.h>
#endif /* NEED_SYS_SYSLOG_H */
int allow_severity = LOG_INFO;
int deny_severity = LOG_WARNING;
#endif /* HAVE_LIBWRAP */

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#if defined(__FreeBSD__) && defined(HAVE_IEEEFP_H)
#include <ieeefp.h>
#ifdef HAVE_FP_EXCEPT				// Fix type conflict
typedef fp_except fp_except_t;
#endif

  /* We can't handle floating point expections with threads, so disable
     this on freebsd
  */

inline void reset_floating_point_exceptions()
{
  /* Don't fall for overflow, underflow,divide-by-zero or loss of precision */
  fpsetmask(~(FP_X_INV | FP_X_DNML | FP_X_OFL | FP_X_UFL |
	      FP_X_DZ | FP_X_IMP));
}
#else
#define reset_floating_point_exceptions()
#endif /* __FreeBSD__ && HAVE_IEEEFP_H */

#ifdef	__cplusplus
}
#endif

#if defined(HAVE_LINUXTHREADS)
#define THR_KILL_SIGNAL SIGINT
#else
#define THR_KILL_SIGNAL SIGUSR2		// Can't use this with LinuxThreads
#endif

#ifdef HAVE_GLIBC2_STYLE_GETHOSTBYNAME_R
#include <sys/types.h>
#else
#include <my_pthread.h>			// For thr_setconcurency()
#endif
#if defined(HAVE_GETRLIMIT) && defined(RLIMIT_NOFILE) && !defined(HAVE_mit_thread)
#define SET_RLIMIT_NOFILE
#endif

#ifdef SOLARIS
extern "C" int gethostname(char *name, int namelen);
#endif

#define MYSQL_KILL_SIGNAL SIGTERM

#ifndef DBUG_OFF
static const char* default_dbug_option=IF_WIN("d:t:i:O,\\mysqld.trace",
					      "d:t:i:o,/tmp/mysqld.trace");
#endif

#ifdef __NT__
static char szPipeName [ 257 ];
static SECURITY_ATTRIBUTES saPipeSecurity;
static SECURITY_DESCRIPTOR sdPipeDescriptor;
static HANDLE hPipe = INVALID_HANDLE_VALUE;
static pthread_cond_t COND_handler_count;
static uint handler_count;
#endif
#ifdef __WIN__
static bool opt_console=0;
#endif

#ifdef HAVE_BERKELEY_DB
SHOW_COMP_OPTION have_berkeley_db=SHOW_OPTION_YES;
#else
SHOW_COMP_OPTION have_berkeley_db=SHOW_OPTION_NO;
#endif
#ifdef HAVE_GEMINI_DB
SHOW_COMP_OPTION have_gemini=SHOW_OPTION_YES;
#else
SHOW_COMP_OPTION have_gemini=SHOW_OPTION_NO;
#endif
#ifdef HAVE_INNOBASE_DB
SHOW_COMP_OPTION have_innobase=SHOW_OPTION_YES;
#else
SHOW_COMP_OPTION have_innobase=SHOW_OPTION_NO;
#endif
#ifndef NO_ISAM
SHOW_COMP_OPTION have_isam=SHOW_OPTION_YES;
#else
SHOW_COMP_OPTION have_isam=SHOW_OPTION_NO;
#endif
#ifdef USE_RAID
SHOW_COMP_OPTION have_raid=SHOW_OPTION_YES;
#else
SHOW_COMP_OPTION have_raid=SHOW_OPTION_NO;
#endif
#ifdef HAVE_OPENSSL
SHOW_COMP_OPTION have_ssl=SHOW_OPTION_YES;
#else
SHOW_COMP_OPTION have_ssl=SHOW_OPTION_NO;
#endif


static bool opt_skip_slave_start = 0; // if set, slave is not autostarted
static ulong opt_specialflag=SPECIAL_ENGLISH;
static my_socket unix_sock= INVALID_SOCKET,ip_sock= INVALID_SOCKET;
static ulong back_log,connect_timeout,concurrency;
static my_string opt_logname=0,opt_update_logname=0,
       opt_binlog_index_name = 0,opt_slow_logname=0;
static char mysql_home[FN_REFLEN],pidfile_name[FN_REFLEN];
static pthread_t select_thread;
static bool opt_log,opt_update_log,opt_bin_log,opt_slow_log,opt_noacl,
	    opt_disable_networking=0, opt_bootstrap=0,opt_skip_show_db=0,
            opt_ansi_mode=0,opt_myisam_log=0,
            opt_large_files=sizeof(my_off_t) > 4;
bool opt_sql_bin_update = 0, opt_log_slave_updates = 0, opt_safe_show_db=0;
FILE *bootstrap_file=0;
int segfaulted = 0; // ensure we do not enter SIGSEGV handler twice
extern MASTER_INFO glob_mi;
extern int init_master_info(MASTER_INFO* mi);

// if sql_bin_update is true, SQL_LOG_UPDATE and SQL_LOG_BIN are kept in sync, and are
// treated as aliases for each other

static bool kill_in_progress=FALSE;
static struct rand_struct sql_rand;
static int cleanup_done;
static char **defaults_argv,time_zone[30];
static const char *default_table_type_name;
static char glob_hostname[FN_REFLEN];

#ifdef HAVE_OPENSSL
static bool opt_use_ssl = FALSE;
static char *opt_ssl_key = 0;
static char *opt_ssl_cert = 0;
static char *opt_ssl_ca = 0;
static char *opt_ssl_capath = 0;
static VioSSLAcceptorFd* ssl_acceptor_fd = 0;
#endif /* HAVE_OPENSSL */


I_List <i_string_pair> replicate_rewrite_db;
I_List<i_string> replicate_do_db, replicate_ignore_db;
// allow the user to tell us which db to replicate and which to ignore
I_List<i_string> binlog_do_db, binlog_ignore_db;

/* if we guessed server_id , we need to know about it */
uint32 server_id = 0;
bool server_id_supplied = 0;

uint mysql_port;
uint test_flags, select_errors=0, dropping_tables=0,ha_open_options=0;
uint volatile thread_count=0, thread_running=0, kill_cached_threads=0,
	      wake_thread=0, global_read_lock=0;
ulong thd_startup_options=(OPTION_UPDATE_LOG | OPTION_AUTO_IS_NULL |
			   OPTION_BIN_LOG | OPTION_QUOTE_SHOW_CREATE );
uint protocol_version=PROTOCOL_VERSION;
ulong keybuff_size,sortbuff_size,max_item_sort_length,table_cache_size,
      max_join_size,join_buff_size,tmp_table_size,thread_stack,
      thread_stack_min,net_wait_timeout,what_to_log= ~ (1L << (uint) COM_TIME),
      query_buff_size, lower_case_table_names, mysqld_net_retry_count,
      net_interactive_timeout, slow_launch_time = 2L,
      net_read_timeout,net_write_timeout,slave_open_temp_tables=0,
      open_files_limit=0;
ulong thread_cache_size=0, binlog_cache_size=0, max_binlog_cache_size=0;
volatile ulong cached_thread_count=0;

// replication parameters, if master_host is not NULL, we are a slave
my_string master_user = (char*) "test", master_password = 0, master_host=0,
  master_info_file = (char*) "master.info";
const char *localhost=LOCAL_HOST;
uint master_port = MYSQL_PORT, master_connect_retry = 60;

ulong max_tmp_tables,max_heap_table_size;
ulong bytes_sent = 0L, bytes_received = 0L;

bool opt_endinfo,using_udf_functions,low_priority_updates, locked_in_memory;
bool volatile abort_loop,select_thread_in_use,grant_option;
bool volatile ready_to_exit,shutdown_in_progress;
ulong refresh_version=1L,flush_version=1L;	/* Increments on each reload */
ulong query_id=1L,long_query_count,long_query_time,aborted_threads,
      aborted_connects,delayed_insert_timeout,delayed_insert_limit,
      delayed_queue_size,delayed_insert_threads,delayed_insert_writes,
      delayed_rows_in_use,delayed_insert_errors,flush_time, thread_created;
ulong filesort_rows, filesort_range_count, filesort_scan_count;
ulong filesort_merge_passes;
ulong select_range_check_count, select_range_count, select_scan_count;
ulong select_full_range_join_count,select_full_join_count;
ulong specialflag=0,opened_tables=0,created_tmp_tables=0,
      created_tmp_disk_tables=0;
ulong max_connections,max_insert_delayed_threads,max_used_connections,
      max_connect_errors;
ulong thread_id=1L,current_pid;
ulong slow_launch_threads = 0;
char mysql_real_data_home[FN_REFLEN],
     mysql_data_home[2],language[LIBLEN],reg_ext[FN_EXTLEN],
     default_charset[LIBLEN],mysql_charsets_dir[FN_REFLEN], *charsets_list,
     blob_newline,f_fyllchar,max_sort_char,*mysqld_user,*mysqld_chroot,
     *opt_init_file;
char *opt_bin_logname = 0; // this one needs to be seen in sql_parse.cc
char server_version[50]=MYSQL_SERVER_VERSION;
const char *first_keyword="first";
const char **errmesg;			/* Error messages */
const char *myisam_recover_options_str="OFF";
my_string mysql_unix_port=NULL,mysql_tmpdir=NULL;
ulong my_bind_addr;			/* the address we bind to */
DATE_FORMAT dayord;
double log_10[32];			/* 10 potences */
I_List<THD> threads,thread_cache;
time_t start_time;


pthread_key(MEM_ROOT*,THR_MALLOC);
pthread_key(THD*, THR_THD);
pthread_key(NET*, THR_NET);
pthread_mutex_t LOCK_mysql_create_db, LOCK_Acl, LOCK_open, LOCK_thread_count,
		LOCK_mapped_file, LOCK_status, LOCK_grant,
		LOCK_error_log,
		LOCK_delayed_insert, LOCK_delayed_status, LOCK_delayed_create,
		LOCK_crypt, LOCK_bytes_sent, LOCK_bytes_received,
                LOCK_binlog_update, LOCK_slave, LOCK_server_id;

pthread_cond_t COND_refresh,COND_thread_count,COND_binlog_update,
  COND_slave_stopped, COND_slave_start;
pthread_cond_t COND_thread_cache,COND_flush_thread_cache;
pthread_t signal_thread;
pthread_attr_t connection_attrib;
enum db_type default_table_type=DB_TYPE_MYISAM;

#ifdef __WIN__
#undef	 getpid
#include <process.h>
HANDLE hEventShutdown;
#include "nt_servc.h"
static	 NTService  Service;	      // Service object for WinNT
#endif

static void *signal_hand(void *arg);
static void set_options(void);
static void get_options(int argc,char **argv);
static char *get_relative_path(const char *path);
static void fix_paths(void);
static pthread_handler_decl(handle_connections_sockets,arg);
static int bootstrap(FILE *file);
static bool read_init_file(char *file_name);
#ifdef __NT__
static pthread_handler_decl(handle_connections_namedpipes,arg);
#endif
#ifdef __WIN__
static int get_service_parameters();
#endif
extern pthread_handler_decl(handle_slave,arg);
#ifdef SET_RLIMIT_NOFILE
static uint set_maximum_open_files(uint max_file_limit);
#endif
static ulong find_bit_type(const char *x, TYPELIB *bit_lib);

/****************************************************************************
** Code to end mysqld
****************************************************************************/

static void close_connections(void)
{
#ifdef EXTRA_DEBUG
  int count=0;
#endif
  NET net;
  DBUG_ENTER("close_connections");

  /* Clear thread cache */
  kill_cached_threads++;
  flush_thread_cache();

  /* kill flush thread */
  (void) pthread_mutex_lock(&LOCK_manager);
  if (manager_thread_in_use)
  {
    DBUG_PRINT("quit",("killing manager thread: %lx",manager_thread));
   (void) pthread_cond_signal(&COND_manager);
  }
  (void) pthread_mutex_unlock(&LOCK_manager);

  /* kill connection thread */
#if !defined(__WIN__) && !defined(__EMX__)
  DBUG_PRINT("quit",("waiting for select thread: %lx",select_thread));
  (void) pthread_mutex_lock(&LOCK_thread_count);

  while (select_thread_in_use)
  {
    struct timespec abstime;
    int error;
    LINT_INIT(error);
#ifndef DONT_USE_THR_ALARM
    if (pthread_kill(select_thread,THR_CLIENT_ALARM))
      break;					// allready dead
#endif
#ifdef HAVE_TIMESPEC_TS_SEC
    abstime.ts_sec=time(NULL)+2;		// Bsd 2.1
    abstime.ts_nsec=0;
#else
    struct timeval tv;
    gettimeofday(&tv,0);
    abstime.tv_sec=tv.tv_sec+2;
    abstime.tv_nsec=tv.tv_usec*1000;
#endif
    for (uint tmp=0 ; tmp < 10 ; tmp++)
    {
      error=pthread_cond_timedwait(&COND_thread_count,&LOCK_thread_count,
				   &abstime);
      if (error != EINTR)
	break;
    }
#ifdef EXTRA_DEBUG
    if (error != 0 && !count++)
      sql_print_error("Got error %d from pthread_cond_timedwait",error);
#endif
#if defined(AIX_3_2) || defined(HAVE_DEC_3_2_THREADS)
    if (ip_sock != INVALID_SOCKET)
    {
      DBUG_PRINT("error",("closing TCP/IP and socket files"));
      VOID(shutdown(ip_sock,2));
      VOID(closesocket(ip_sock));
      VOID(shutdown(unix_sock,2));
      VOID(closesocket(unix_sock));
      VOID(unlink(mysql_unix_port));
      ip_sock=unix_sock= INVALID_SOCKET;
    }
#endif
  }
  (void) pthread_mutex_unlock(&LOCK_thread_count);
#endif /* __WIN__ */


  /* Abort listening to new connections */
  DBUG_PRINT("quit",("Closing sockets"));
  if ( !opt_disable_networking )
  {
    if (ip_sock != INVALID_SOCKET)
    {
      (void) shutdown(ip_sock,2);
      (void) closesocket(ip_sock);
      ip_sock= INVALID_SOCKET;
    }
  }
#ifdef __NT__
  if ( hPipe != INVALID_HANDLE_VALUE )
  {
    HANDLE hTempPipe = hPipe;
    DBUG_PRINT( "quit", ("Closing named pipes") );
    hPipe = INVALID_HANDLE_VALUE;
    CancelIo( hTempPipe );
    DisconnectNamedPipe( hTempPipe );
    CloseHandle( hTempPipe );
  }
#endif
#ifdef HAVE_SYS_UN_H
  if (unix_sock != INVALID_SOCKET)
  {
    (void) shutdown(unix_sock,2);
    (void) closesocket(unix_sock);
    (void) unlink(mysql_unix_port);
    unix_sock= INVALID_SOCKET;
  }
#endif
  end_thr_alarm();			 // Don't allow alarms

  /* First signal all threads that it's time to die */

  THD *tmp;
  (void) pthread_mutex_lock(&LOCK_thread_count); // For unlink from list

  I_List_iterator<THD> it(threads);
  while ((tmp=it++))
  {
    DBUG_PRINT("quit",("Informing thread %ld that it's time to die",
		       tmp->thread_id));
    tmp->killed=1;
    if (tmp->mysys_var)
    {
      tmp->mysys_var->abort=1;
      if (tmp->mysys_var->current_mutex)
      {
	pthread_mutex_lock(tmp->mysys_var->current_mutex);
	pthread_cond_broadcast(tmp->mysys_var->current_cond);
	pthread_mutex_unlock(tmp->mysys_var->current_mutex);
      }
    }
  }
  (void) pthread_mutex_unlock(&LOCK_thread_count); // For unlink from list

  if (thread_count)
  {
    sleep(1);					// Give threads time to die
  }

  /* Force remaining threads to die by closing the connection to the client */

  (void) my_net_init(&net, (Vio*) 0);
  for (;;)
  {
    DBUG_PRINT("quit",("Locking LOCK_thread_count"));
    (void) pthread_mutex_lock(&LOCK_thread_count); // For unlink from list
    if (!(tmp=threads.get()))
    {
      DBUG_PRINT("quit",("Unlocking LOCK_thread_count"));
      (void) pthread_mutex_unlock(&LOCK_thread_count);
      break;
    }
#ifndef __bsdi__				// Bug in BSDI kernel
    if ((net.vio=tmp->net.vio) != 0)
    {
      sql_print_error(ER(ER_FORCING_CLOSE),my_progname,
		      tmp->thread_id,tmp->user ? tmp->user : "");
      close_connection(&net,0,0);
    }
#endif
    DBUG_PRINT("quit",("Unlocking LOCK_thread_count"));
    (void) pthread_mutex_unlock(&LOCK_thread_count);
  }
  net_end(&net);
  /* All threads has now been aborted */
  DBUG_PRINT("quit",("Waiting for threads to die (count=%u)",thread_count));
  (void) pthread_mutex_lock(&LOCK_thread_count);
  while (thread_count)
  {
    (void) pthread_cond_wait(&COND_thread_count,&LOCK_thread_count);
    DBUG_PRINT("quit",("One thread died (count=%u)",thread_count));
  }
  (void) pthread_mutex_unlock(&LOCK_thread_count);

  mysql_log.close(1);
  mysql_update_log.close(1);
  mysql_bin_log.close(1);
  my_free(charsets_list, MYF(0));
  DBUG_PRINT("quit",("close_connections thread"));
  DBUG_VOID_RETURN;
}

void kill_mysql(void)
{
  DBUG_ENTER("kill_mysql");

#if defined(__WIN__)
  {
    if (!SetEvent(hEventShutdown))
    {
      DBUG_PRINT("error",("Got error: %ld from SetEvent",GetLastError()));
    }
    // or:
    // HANDLE hEvent=OpenEvent(0, FALSE, "MySqlShutdown");
    // SetEvent(hEventShutdown);
    // CloseHandle(hEvent);
  }
#elif defined(HAVE_PTHREAD_KILL)
    if (pthread_kill(signal_thread,SIGTERM))	/* End everything nicely */
    {
      DBUG_PRINT("error",("Got error %d from pthread_kill",errno)); /* purecov: inspected */
    }
#else
    kill(current_pid,SIGTERM);
#endif
    DBUG_PRINT("quit",("After pthread_kill"));
    shutdown_in_progress=1;			// Safety if kill didn't work
    DBUG_VOID_RETURN;
}


	/* Force server down. kill all connections and threads and exit */

#ifndef __WIN__
static void *kill_server(void *sig_ptr)
#define RETURN_FROM_KILL_SERVER return 0
#else
static void __cdecl kill_server(int sig_ptr)
#define RETURN_FROM_KILL_SERVER return
#endif
{
  int sig=(int) (long) sig_ptr;			// This is passed a int
  DBUG_ENTER("kill_server");

  // if there is a signal during the kill in progress, we do not need
  // another one
  if (kill_in_progress)				// Safety
    RETURN_FROM_KILL_SERVER;
  kill_in_progress=TRUE;
  abort_loop=1;					// This should be set
  signal(sig,SIG_IGN);
  if (sig == MYSQL_KILL_SIGNAL || sig == 0)
    sql_print_error(ER(ER_NORMAL_SHUTDOWN),my_progname);
  else
    sql_print_error(ER(ER_GOT_SIGNAL),my_progname,sig); /* purecov: inspected */

#if defined(USE_ONE_SIGNAL_HAND) && !defined(__WIN__)
  my_thread_init();				// If this is a new thread
#endif
  close_connections();
  sql_print_error(ER(ER_SHUTDOWN_COMPLETE),my_progname);
  if (sig != MYSQL_KILL_SIGNAL && sig != 0)
    unireg_abort(1);				/* purecov: inspected */
  else
    unireg_end(0);
  pthread_exit(0);				/* purecov: deadcode */
  RETURN_FROM_KILL_SERVER;
}


#ifdef USE_ONE_SIGNAL_HAND
pthread_handler_decl(kill_server_thread,arg __attribute__((unused)))
{
  my_thread_init();				// Initialize new thread
  kill_server(0);
  my_thread_end();				// Normally never reached
  return 0;
}
#endif

static sig_handler print_signal_warning(int sig)
{
  sql_print_error("Warning: Got signal %d from thread %d",
		  sig,my_thread_id());
#ifdef DONT_REMEMBER_SIGNAL
  sigset(sig,print_signal_warning);		/* int. thread system calls */
#endif
#ifndef __WIN__
  if (sig == SIGALRM)
    alarm(2);					/* reschedule alarm */
#endif
}


void unireg_end(int signal_number __attribute__((unused)))
{
  clean_up();
  pthread_exit(0);				// Exit is in main thread
}


void unireg_abort(int exit_code)
{
  if (exit_code)
    sql_print_error("Aborting\n");
  clean_up(); /* purecov: inspected */
  exit(exit_code); /* purecov: inspected */
}


void clean_up(void)
{
  DBUG_PRINT("exit",("clean_up"));
  if (cleanup_done++)
    return; /* purecov: inspected */
  acl_free(1);
  grant_free();
  sql_cache_free();
  table_cache_free();
  hostname_cache_free();
  item_user_lock_free();
  lex_free();				/* Free some memory */
#ifdef HAVE_DLOPEN
  if (!opt_noacl)
    udf_free();
#endif
  end_key_cache();			/* This is usually freed automaticly */
  (void) ha_panic(HA_PANIC_CLOSE);	/* close all tables and logs */
#ifdef USE_RAID
  end_raid();
#endif
  x_free((gptr) errmsg[ERRMAPP]);	/* Free messages */
  free_defaults(defaults_argv);
  my_free(mysql_tmpdir,MYF(0));
  x_free(opt_bin_logname);
#ifndef __WIN__
  if (!opt_bootstrap)
    (void) my_delete(pidfile_name,MYF(0));	// This may not always exist
#endif
  end_slave();
  my_thread_end();

  /* Tell main we are ready */
  (void) pthread_mutex_lock(&LOCK_thread_count);
  ready_to_exit=1;
  /* do the broadcast inside the lock to ensure that my_end() is not called */
  (void) pthread_cond_broadcast(&COND_thread_count);
  (void) pthread_mutex_unlock(&LOCK_thread_count);
} /* clean_up */



/****************************************************************************
** Init IP and UNIX socket
****************************************************************************/

static void set_ports()
{
  char	*env;
  if (!mysql_port && !opt_disable_networking)
  {					// Get port if not from commandline
    struct  servent *serv_ptr;
    mysql_port = MYSQL_PORT;
    if ((serv_ptr = getservbyname("mysql", "tcp")))
      mysql_port = ntohs((u_short) serv_ptr->s_port); /* purecov: inspected */
    if ((env = getenv("MYSQL_TCP_PORT")))
      mysql_port = (uint) atoi(env);		/* purecov: inspected */
  }
  if (!mysql_unix_port)
  {
#ifdef __WIN__
    mysql_unix_port = (char*) MYSQL_NAMEDPIPE;
#else
    mysql_unix_port = (char*) MYSQL_UNIX_ADDR;
#endif
    if ((env = getenv("MYSQL_UNIX_PORT")))
      mysql_unix_port = env;			/* purecov: inspected */
  }
}

/* Change to run as another user if started with --user */

static void set_user(const char *user)
{
#ifndef __WIN__
    struct passwd *ent;

  // don't bother if we aren't superuser
  if (geteuid())
  {
    if (user)
      fprintf(stderr,
	      "Warning: One can only use the --user switch if running as root\n");
    return;
  }
  else if (!user)
  {
    if (!opt_bootstrap)
    {
      fprintf(stderr,"Fatal error: Please read \"Security\" section of the manual to find out how to run mysqld as root!\n");
      unireg_abort(1);
    }
    return;
  }
  if (!strcmp(user,"root"))
    return;				// Avoid problem with dynamic libraries

  if (!(ent = getpwnam(user)))
  {
    fprintf(stderr,"Fatal error: Can't change to run as user '%s' ;  Please check that the user exists!\n",user);
    unireg_abort(1);
  }
#ifdef HAVE_INITGROUPS
  initgroups((char*) user,ent->pw_gid);
#endif
  if (setgid(ent->pw_gid) == -1)
  {
    sql_perror("setgid");
    unireg_abort(1);
  }
  if (setuid(ent->pw_uid) == -1)
  {
    sql_perror("setuid");
    unireg_abort(1);
  }
#endif
}

/* Change root user if started with  --chroot */

static void set_root(const char *path)
{
#if !defined(__WIN__) && !defined(__EMX__)
  if (chroot(path) == -1)
  {
    sql_perror("chroot");
    unireg_abort(1);
  }
#endif
}

static void server_init(void)
{
  struct sockaddr_in	IPaddr;
#ifdef HAVE_SYS_UN_H
  struct sockaddr_un	UNIXaddr;
#endif
  int	arg=1;
  DBUG_ENTER("server_init");

#ifdef	__WIN__
  if ( !opt_disable_networking )
  {
    WSADATA WsaData;
    if (SOCKET_ERROR == WSAStartup (0x0101, &WsaData))
    {
      my_message(0,"WSAStartup Failed\n",MYF(0));
      unireg_abort(1);
    }
  }
#endif /* __WIN__ */

  set_ports();

  if (mysql_port != 0 && !opt_disable_networking && !opt_bootstrap)
  {
    DBUG_PRINT("general",("IP Socket is %d",mysql_port));
    ip_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ip_sock == INVALID_SOCKET)
    {
      DBUG_PRINT("error",("Got error: %d from socket()",socket_errno));
      sql_perror(ER(ER_IPSOCK_ERROR));		/* purecov: tested */
      unireg_abort(1);				/* purecov: tested */
    }
    bzero((char*) &IPaddr, sizeof(IPaddr));
    IPaddr.sin_family = AF_INET;
    IPaddr.sin_addr.s_addr = my_bind_addr;
    IPaddr.sin_port = (unsigned short) htons((unsigned short) mysql_port);
    (void) setsockopt(ip_sock,SOL_SOCKET,SO_REUSEADDR,(char*)&arg,sizeof(arg));
    for(;;)
    {
      if (bind(ip_sock, my_reinterpret_cast(struct sockaddr *) (&IPaddr),
	       sizeof(IPaddr)) >= 0)
	break;
      DBUG_PRINT("error",("Got error: %d from bind",socket_errno));
      sql_perror("Can't start server: Bind on TCP/IP port");/* Had a loop here */
      sql_print_error("Do you already have another mysqld server running on port: %d ?",mysql_port);
      unireg_abort(1);
    }
    if (listen(ip_sock,(int) back_log) < 0)
      sql_print_error("Warning:  listen() on TCP/IP failed with error %d",
		      errno);
  }

  if (mysqld_chroot)
    set_root(mysqld_chroot);

  set_user(mysqld_user); // set_user now takes care of mysqld_user==NULL

#ifdef __NT__
  /* create named pipe */
  if (Service.IsNT() && mysql_unix_port[0] && !opt_bootstrap)
  {
    sprintf( szPipeName, "\\\\.\\pipe\\%s", mysql_unix_port );
    ZeroMemory( &saPipeSecurity, sizeof(saPipeSecurity) );
    ZeroMemory( &sdPipeDescriptor, sizeof(sdPipeDescriptor) );
    if ( !InitializeSecurityDescriptor(&sdPipeDescriptor,
				       SECURITY_DESCRIPTOR_REVISION) )
    {
      sql_perror("Can't start server : Initialize security descriptor");
      unireg_abort(1);
    }
    if (!SetSecurityDescriptorDacl(&sdPipeDescriptor, TRUE, NULL, FALSE))
    {
      sql_perror("Can't start server : Set security descriptor");
      unireg_abort(1);
    }
    saPipeSecurity.nLength = sizeof( SECURITY_ATTRIBUTES );
    saPipeSecurity.lpSecurityDescriptor = &sdPipeDescriptor;
    saPipeSecurity.bInheritHandle = FALSE;
    if ((hPipe = CreateNamedPipe(szPipeName,
				 PIPE_ACCESS_DUPLEX,
				 PIPE_TYPE_BYTE |
				 PIPE_READMODE_BYTE |
				 PIPE_WAIT,
				 PIPE_UNLIMITED_INSTANCES,
				 (int) net_buffer_length,
				 (int) net_buffer_length,
				 NMPWAIT_USE_DEFAULT_WAIT,
				 &saPipeSecurity )) == INVALID_HANDLE_VALUE)
      {
	LPVOID lpMsgBuf;
	int error=GetLastError();
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
		      FORMAT_MESSAGE_FROM_SYSTEM,
		      NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		      (LPTSTR) &lpMsgBuf, 0, NULL );
	MessageBox( NULL, (LPTSTR) lpMsgBuf, "Error from CreateNamedPipe",
		    MB_OK|MB_ICONINFORMATION );
	LocalFree( lpMsgBuf );
	unireg_abort(1);
      }
  }
#endif

#if defined(HAVE_SYS_UN_H)
  /*
  ** Create the UNIX socket
  */
  if (mysql_unix_port[0] && !opt_bootstrap)
  {
    DBUG_PRINT("general",("UNIX Socket is %s",mysql_unix_port));

    if ((unix_sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
    {
      sql_perror("Can't start server : UNIX Socket "); /* purecov: inspected */
      unireg_abort(1);				/* purecov: inspected */
    }
    bzero((char*) &UNIXaddr, sizeof(UNIXaddr));
    UNIXaddr.sun_family = AF_UNIX;
    strmov(UNIXaddr.sun_path, mysql_unix_port);
    (void) unlink(mysql_unix_port);
    (void) setsockopt(unix_sock,SOL_SOCKET,SO_REUSEADDR,(char*)&arg,
		      sizeof(arg));
    umask(0);
    if (bind(unix_sock, my_reinterpret_cast(struct sockaddr *) (&UNIXaddr),
	     sizeof(UNIXaddr)) < 0)
    {
      sql_perror("Can't start server : Bind on unix socket"); /* purecov: tested */
      sql_print_error("Do you already have another mysqld server running on socket: %s ?",mysql_unix_port);
      unireg_abort(1);					/* purecov: tested */
    }
    umask(((~my_umask) & 0666));
#if defined(S_IFSOCK) && defined(SECURE_SOCKETS)
    (void) chmod(mysql_unix_port,S_IFSOCK);	/* Fix solaris 2.6 bug */
#endif
    if (listen(unix_sock,(int) back_log) < 0)
      sql_print_error("Warning:  listen() on Unix socket failed with error %d",
		      errno);
  }
#endif
  DBUG_PRINT("info",("server started"));
  DBUG_VOID_RETURN;
}


void yyerror(const char *s)
{
  NET *net=my_pthread_getspecific_ptr(NET*,THR_NET);
  char *yytext=(char*) current_lex->tok_start;
  if (!strcmp(s,"parse error"))
    s=ER(ER_SYNTAX_ERROR);
  net_printf(net,ER_PARSE_ERROR, s, yytext ? (char*) yytext : "",
	     current_lex->yylineno);
}


void close_connection(NET *net,uint errcode,bool lock)
{
  Vio* vio;
  DBUG_ENTER("close_connection");
  DBUG_PRINT("enter",("fd: %s  error: '%s'",
                    net->vio? vio_description(net->vio):"(not connected)",
                    errcode ? ER(errcode) : ""));
  if (lock)
    (void) pthread_mutex_lock(&LOCK_thread_count);
  if ((vio=net->vio) != 0)
  {
    if (errcode)
      send_error(net,errcode,ER(errcode));	/* purecov: inspected */
    vio_close(vio);			/* vio is freed in delete thd */
  }
  if (lock)
    (void) pthread_mutex_unlock(&LOCK_thread_count);
  DBUG_VOID_RETURN;
}

	/* Called when a thread is aborted */
	/* ARGSUSED */

sig_handler end_thread_signal(int sig __attribute__((unused)))
{
  THD *thd=current_thd;
  DBUG_ENTER("end_thread_signal");
  if (thd)
    end_thread(thd,0);
  DBUG_VOID_RETURN;				/* purecov: deadcode */
}


void end_thread(THD *thd, bool put_in_cache)
{
  DBUG_ENTER("end_thread");
  (void) pthread_mutex_lock(&LOCK_thread_count);
  thread_count--;
  delete thd;

  if (put_in_cache && cached_thread_count < thread_cache_size &&
      ! abort_loop && !kill_cached_threads)
  {
    /* Don't kill the thread, just put it in cache for reuse */
    DBUG_PRINT("info", ("Adding thread to cache"))
    cached_thread_count++;
    while (!abort_loop && ! wake_thread && ! kill_cached_threads)
      (void) pthread_cond_wait(&COND_thread_cache, &LOCK_thread_count);
    cached_thread_count--;
    if (kill_cached_threads)
      pthread_cond_signal(&COND_flush_thread_cache);
    if (wake_thread)
    {
      wake_thread--;
      thd=thread_cache.get();
      thd->real_id=pthread_self();
      (void) thd->store_globals();
      threads.append(thd);
      pthread_mutex_unlock(&LOCK_thread_count);
      DBUG_VOID_RETURN;
    }
  }

  DBUG_PRINT("info", ("sending a broadcast"))

  /* Tell main we are ready */
  (void) pthread_mutex_unlock(&LOCK_thread_count);
  (void) pthread_cond_broadcast(&COND_thread_count);
  DBUG_PRINT("info", ("unlocked thread_count mutex"))
#ifdef ONE_THREAD
  if (!(test_flags & TEST_NO_THREADS))	// For debugging under Linux
#endif
  {
    my_thread_end();
    pthread_exit(0);
  }
  DBUG_VOID_RETURN;
}


/* Start a cached thread. LOCK_thread_count is locked on entry */

static void start_cached_thread(THD *thd)
{
  thread_cache.append(thd);
  wake_thread++;
  thread_count++;
  pthread_cond_signal(&COND_thread_cache);
}


void flush_thread_cache()
{
  (void) pthread_mutex_lock(&LOCK_thread_count);
  kill_cached_threads++;
  while (cached_thread_count)
  {
    pthread_cond_broadcast(&COND_thread_cache);
    pthread_cond_wait(&COND_flush_thread_cache,&LOCK_thread_count);
  }
  kill_cached_threads--;
  (void) pthread_mutex_unlock(&LOCK_thread_count);
}


	/*
	** Aborts a thread nicely. Commes here on SIGPIPE
	** TODO: One should have to fix that thr_alarm know about this
	** thread too
	*/

#ifdef THREAD_SPECIFIC_SIGPIPE
static sig_handler abort_thread(int sig __attribute__((unused)))
{
  THD *thd=current_thd;
  DBUG_ENTER("abort_thread");
  if (thd)
    thd->killed=1;
  DBUG_VOID_RETURN;
}
#endif

/******************************************************************************
** Setup a signal thread with handles all signals
** Because linux doesn't support scemas use a mutex to check that
** the signal thread is ready before continuing
******************************************************************************/

#ifdef __WIN__
static void init_signals(void)
{
  int signals[] = {SIGINT,SIGILL,SIGFPE,SIGSEGV,SIGTERM,SIGABRT } ;
  for (uint i=0 ; i < sizeof(signals)/sizeof(int) ; i++)
    signal( signals[i], kill_server) ;
  signal(SIGBREAK,SIG_IGN);	//ignore SIGBREAK for NT
}

#elif defined(__EMX__)
static void sig_reload(int signo)
{
  //reload_acl_and_cache(~0);		// Flush everything
  reload_acl_and_cache((THD*) 0,~0, (TABLE_LIST*) 0); // Flush everything
  signal(signo, SIG_ACK);
}

static void sig_kill(int signo)
{
  if (!abort_loop)
  {
    abort_loop=1;				// mark abort for threads
    kill_server((void*) signo);
  }
  signal(signo, SIG_ACK);
}

static void init_signals(void)
{
  signal(SIGQUIT, sig_kill);
  signal(SIGKILL, sig_kill);
  signal(SIGTERM, sig_kill);
  signal(SIGINT,  sig_kill);
  signal(SIGHUP,  sig_reload);	// Flush everything
  signal(SIGALRM, SIG_IGN);
  signal(SIGBREAK,SIG_IGN);
  signal_thread = pthread_self();
}
#else

#ifdef HAVE_LINUXTHREADS
static sig_handler write_core(int sig);
#ifdef __i386__
#define SIGRETURN_FRAME_COUNT  1
inline static __volatile__ void  trace_stack()
{
  uchar **stack_bottom;
  uchar** ebp;
  LINT_INIT(ebp);
  LINT_INIT(stack_bottom);

  fprintf(stderr,
"Attemping backtrace. You can use the following information to find out\n\
where mysqld died.  If you see no messages after this, something went\n\
terribly wrong\n");
  THD* thd = current_thd;
  uint frame_count = 0;
  __asm __volatile__ ("movl %%ebp,%0"
		      :"=r"(ebp)
		      :"r"(ebp));
  if (!ebp)
  {
    fprintf(stderr, "frame pointer (ebp) is NULL, did you compile with\n\
-fomit-frame-pointer? Aborting backtrace\n");
    return;
  }
  if (!thd)
  {
    fprintf(stderr, "Cannot determine thread, ebp=%p, backtrace may not be correct\n", ebp);
    /* Assume that the stack starts at the previous even 65K */
    ulong tmp= min(0x10000,thread_stack);
    stack_bottom= (uchar**) (((ulong) &stack_bottom + tmp) &
			     ~(ulong) 0xFFFF);
  }
  else
    stack_bottom = (uchar**) thd->thread_stack;
  if (ebp > stack_bottom || ebp < stack_bottom - thread_stack)
  {
    fprintf(stderr,
	    "Bogus stack limit or frame pointer, aborting backtrace\n");
    return;
  }

  fprintf(stderr, "stack range sanity check, ok, backtrace follows\n");
  
  while (ebp < stack_bottom)
  {
    uchar** new_ebp = (uchar**)*ebp;
    fprintf(stderr, "%p\n", frame_count == SIGRETURN_FRAME_COUNT ?
	    *(ebp+17) : *(ebp+1));
    if (new_ebp <= ebp )
    {
      fprintf(stderr, "\
New value of ebp failed sanity check terminating backtrace\n");
      return;
    }
    ebp = new_ebp;
    ++frame_count;
  }

  fprintf(stderr, "stack trace successful\n"); 
}
#endif
#endif

static sig_handler handle_segfault(int sig)
{
  // strictly speaking, one needs a mutex here
  // but since we have got SIGSEGV already, things are a mess
  // so not having the mutex is not as bad as possibly using a buggy
  // mutex - so we keep things simple
  if (segfaulted)
    return;
  segfaulted = 1;
  fprintf(stderr,"\
mysqld got signal %d;\n\
The manual section 'Debugging a MySQL server' tells you how to use a\n\
stack trace and/or the core file to produce a readable backtrace that may\n\
help in finding out why mysqld died\n",sig);
#if defined(HAVE_LINUXTHREADS)
#ifdef __i386__
  trace_stack();
#endif /* __i386__ */
 if (test_flags & TEST_CORE_ON_SIGNAL)
   write_core(sig);
#endif /* HAVE_LINUXTHREADS */
 exit(1);
}

/* Produce a core for the thread */

#ifdef HAVE_LINUXTHREADS
static sig_handler write_core(int sig)
{
  signal(sig, SIG_DFL);
  if (fork() != 0) exit(1);			// Abort main program
  // Core will be written at exit
}
#endif


static void init_signals(void)
{
  sigset_t set;
  pthread_attr_t thr_attr;
  int error;
  DBUG_ENTER("init_signals");

  sigset(THR_KILL_SIGNAL,end_thread_signal);
  sigset(THR_SERVER_ALARM,print_signal_warning); // Should never be called!
  struct sigaction sa; sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  sigprocmask(SIG_SETMASK,&sa.sa_mask,NULL);
#ifdef HAVE_DARWIN_THREADS
   sa.sa_handler=( void (*)() ) handle_segfault;
#else
   sa.sa_handler=handle_segfault;
#endif
  sigaction(SIGSEGV, &sa, NULL);
  (void) sigemptyset(&set);
#ifdef THREAD_SPECIFIC_SIGPIPE
  sigset(SIGPIPE,abort_thread);
  sigaddset(&set,SIGPIPE);
#else
  (void) signal(SIGPIPE,SIG_IGN);		// Can't know which thread
  sigaddset(&set,SIGPIPE);
#endif
  sigaddset(&set,SIGINT);
  sigaddset(&set,SIGQUIT);
  sigaddset(&set,SIGTERM);
  sigaddset(&set,SIGHUP);
  signal(SIGTERM,SIG_DFL);			// If it's blocked by parent
  signal(SIGHUP,SIG_DFL);			// If it's blocked by parent
#ifdef SIGTSTP
  sigaddset(&set,SIGTSTP);
#endif
  sigaddset(&set,THR_SERVER_ALARM);
  sigdelset(&set,THR_KILL_SIGNAL);		// May be SIGINT
  sigdelset(&set,THR_CLIENT_ALARM);		// For alarms
  (void) pthread_sigmask(SIG_SETMASK,&set,NULL);

  (void) pthread_attr_init(&thr_attr);
#if !defined(HAVE_DEC_3_2_THREADS)
  pthread_attr_setscope(&thr_attr,PTHREAD_SCOPE_SYSTEM);
  (void) pthread_attr_setdetachstate(&thr_attr,PTHREAD_CREATE_DETACHED);
  if (!(opt_specialflag & SPECIAL_NO_PRIOR))
    my_pthread_attr_setprio(&thr_attr,INTERRUPT_PRIOR);
  pthread_attr_setstacksize(&thr_attr,32768);
#endif

  (void) pthread_mutex_lock(&LOCK_thread_count);
  if ((error=pthread_create(&signal_thread,&thr_attr,signal_hand,0)))
  {
    sql_print_error("Can't create interrupt-thread (error %d, errno: %d)",
		    error,errno);
    exit(1);
  }
  (void) pthread_cond_wait(&COND_thread_count,&LOCK_thread_count);
  pthread_mutex_unlock(&LOCK_thread_count);

  (void) pthread_attr_destroy(&thr_attr);
  DBUG_VOID_RETURN;
}


/*
** This threads handles all signals and alarms
*/

/* ARGSUSED */
static void *signal_hand(void *arg __attribute__((unused)))
{
  sigset_t set;
  int sig;
  my_thread_init();				// Init new thread
  DBUG_ENTER("signal_hand");

  /* Setup alarm handler */
  init_thr_alarm(max_connections+max_insert_delayed_threads);
#if SIGINT != THR_KILL_SIGNAL
  (void) sigemptyset(&set);			// Setup up SIGINT for debug
  (void) sigaddset(&set,SIGINT);		// For debugging
  (void) pthread_sigmask(SIG_UNBLOCK,&set,NULL);
#endif
  (void) sigemptyset(&set);			// Setup up SIGINT for debug
#ifdef USE_ONE_SIGNAL_HAND
  (void) sigaddset(&set,THR_SERVER_ALARM);	// For alarms
#endif
  (void) sigaddset(&set,SIGQUIT);
  (void) sigaddset(&set,SIGTERM);
#if THR_CLIENT_ALARM != SIGHUP
  (void) sigaddset(&set,SIGHUP);
#endif
  (void) sigaddset(&set,SIGTSTP);

  /* Save pid to this process (or thread on Linux) */
  if (!opt_bootstrap)
  {
    File pidFile;
    if ((pidFile = my_create(pidfile_name,0664, O_WRONLY, MYF(MY_WME))) >= 0)
    {
      char buff[21];
      sprintf(buff,"%lu",(ulong) getpid());
      (void) my_write(pidFile, buff,strlen(buff),MYF(MY_WME));
      (void) my_close(pidFile,MYF(0));
    }
  }

  (void) pthread_mutex_lock(&LOCK_thread_count);
  (void) pthread_cond_signal(&COND_thread_count); /* continue init_signals */
  (void) pthread_mutex_unlock(&LOCK_thread_count);

  for (;;)
  {
    int error;					// Used when debugging
    if (shutdown_in_progress && !abort_loop)
    {
      sig=SIGTERM;
      error=0;
    }
    else
      while ((error=my_sigwait(&set,&sig)) == EINTR) ;
    if (cleanup_done)
      pthread_exit(0);				// Safety
    switch (sig) {
    case SIGTERM:
    case SIGQUIT:
    case SIGKILL:
#ifdef EXTRA_DEBUG
      sql_print_error("Got signal %d to shutdown mysqld",sig);
#endif
      DBUG_PRINT("info",("Got signal: %d  abort_loop: %d",sig,abort_loop));
      if (!abort_loop)
      {
	abort_loop=1;				// mark abort for threads
#ifdef USE_ONE_SIGNAL_HAND
	pthread_t tmp;
	if (!(opt_specialflag & SPECIAL_NO_PRIOR))
	  my_pthread_attr_setprio(&connection_attrib,INTERRUPT_PRIOR);
	if (pthread_create(&tmp,&connection_attrib, kill_server_thread,
			   (void*) sig))
	  sql_print_error("Error: Can't create thread to kill server");
#else
	  kill_server((void*) sig);		// MIT THREAD has a alarm thread
#endif
      }
      break;
    case SIGHUP:
      reload_acl_and_cache((THD*) 0,~0, (TABLE_LIST*) 0); // Flush everything
      mysql_print_status((THD*) 0);		// Send debug some info
      break;
#ifdef USE_ONE_SIGNAL_HAND
    case THR_SERVER_ALARM:
      process_alarm(sig);			// Trigger alarms.
      break;
#endif
    default:
#ifdef EXTRA_DEBUG
      sql_print_error("Warning: Got signal: %d, error: %d",sig,error); /* purecov: tested */
#endif
      break;					/* purecov: tested */
    }
  }
  return(0);					/* purecov: deadcode */
}

#endif	/* __WIN__*/


/*
** All global error messages are sent here where the first one is stored for
** the client
*/


/* ARGSUSED */
static int my_message_sql(uint error, const char *str,
			  myf MyFlags __attribute__((unused)))
{
  NET *net;
  DBUG_ENTER("my_message_sql");
  DBUG_PRINT("error",("Message: '%s'",str));
  if ((net=my_pthread_getspecific_ptr(NET*,THR_NET)))
  {
    if (!net->last_error[0])			// Return only first message
    {
      strmake(net->last_error,str,sizeof(net->last_error)-1);
      net->last_errno=error ? error : ER_UNKNOWN_ERROR;
    }
  }
  else
    sql_print_error("%s: %s",my_progname,str); /* purecov: inspected */
  DBUG_RETURN(0);
}

#ifdef __WIN__
#undef errno
#undef EINTR
#define errno WSAGetLastError()
#define EINTR WSAEINTR

struct utsname
{
  char nodename[FN_REFLEN];
};

int uname(struct utsname *a)
{
  return -1;
}
#endif


#ifdef __WIN__
pthread_handler_decl(handle_shutdown,arg)
{
  MSG msg;
  my_thread_init();

  /* this call should create the message queue for this thread */
  PeekMessage(&msg, NULL, 1, 65534,PM_NOREMOVE);

  if (WaitForSingleObject(hEventShutdown,INFINITE)==WAIT_OBJECT_0)
     kill_server(MYSQL_KILL_SIGNAL);
  return 0;
}

int __stdcall handle_kill(ulong ctrl_type)
{
  if (ctrl_type == CTRL_CLOSE_EVENT ||
      ctrl_type == CTRL_SHUTDOWN_EVENT)
  {
    kill_server(MYSQL_KILL_SIGNAL);
    return TRUE;
  }
  return FALSE;
}
#endif

const char *load_default_groups[]= { "mysqld","server",0 };

#ifdef HAVE_LIBWRAP
char *libwrapName=NULL;
#endif

static void open_log(MYSQL_LOG *log, const char *hostname,
		     const char *opt_name, const char *extension,
		     enum_log_type type)
{
  char tmp[FN_REFLEN];
  if (!opt_name || !opt_name[0])
  {
    strnmov(tmp,hostname,FN_REFLEN-5);
    strmov(strcend(tmp,'.'),extension);
    opt_name=tmp;
  }
  log->open(opt_name,type);
}



#ifdef __WIN__
int win_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
  DEBUGGER_OFF;

  my_umask=0660;		// Default umask for new files
  my_umask_dir=0700;		// Default umask for new directories
  MY_INIT(argv[0]);		// init my_sys library & pthreads
  tzset();			// Set tzname

  start_time=time((time_t*) 0);
#ifdef HAVE_TZNAME
#if defined(HAVE_LOCALTIME_R) && defined(_REENTRANT)
  {
    struct tm tm_tmp;
    localtime_r(&start_time,&tm_tmp);
    strmov(time_zone,tzname[tm_tmp.tm_isdst == 1 ? 1 : 0]);
  }
#else
  {
    struct tm *start_tm;
    start_tm=localtime(&start_time);
    strmov(time_zone,tzname[start_tm->tm_isdst == 1 ? 1 : 0]);
  }
#endif
#endif

  if (gethostname(glob_hostname,sizeof(glob_hostname)-4) < 0)
    strmov(glob_hostname,"mysql");
  strmov(pidfile_name,glob_hostname);
  strmov(strcend(pidfile_name,'.'),".pid");	// Add extension
#ifdef DEMO_VERSION
  strcat(server_version,"-demo");
#endif
#ifdef SHAREWARE_VERSION
  strcat(server_version,"-shareware");
#endif
#ifndef DBUG_OFF
  strcat(server_version,"-debug");
#endif
#ifdef _CUSTOMSTARTUPCONFIG_
  if (_cust_check_startup())
  {
    /* _cust_check_startup will report startup failure error */
    exit( 1 );
  }
#endif
  load_defaults("my",load_default_groups,&argc,&argv);
  defaults_argv=argv;
  mysql_tmpdir=getenv("TMPDIR");	/* Use this if possible */
#ifdef __WIN__
  if (!mysql_tmpdir)
    mysql_tmpdir=getenv("TEMP");
  if (!mysql_tmpdir)
    mysql_tmpdir=getenv("TMP");
#endif
  if (!mysql_tmpdir || !mysql_tmpdir[0])
    mysql_tmpdir=(char*) P_tmpdir;		/* purecov: inspected */

  set_options();
#ifdef __WIN__
  /* service parameters can be overwritten by options */
  if (get_service_parameters())
  {
    my_message( 0, "Can't read MySQL service parameters", MYF(0) );
    exit( 1 );
  }
#endif
  get_options(argc,argv);
  if (opt_log || opt_update_log || opt_slow_log || opt_bin_log)
    strcat(server_version,"-log");
  DBUG_PRINT("info",("%s  Ver %s for %s on %s\n",my_progname,
		     server_version, SYSTEM_TYPE,MACHINE_TYPE));

  /* These must be set early */

  (void) pthread_cond_init(&COND_thread_count,NULL);
  (void) pthread_mutex_init(&LOCK_mysql_create_db,NULL);
  (void) pthread_mutex_init(&LOCK_Acl,NULL);
  (void) pthread_mutex_init(&LOCK_grant,NULL);
  (void) pthread_mutex_init(&LOCK_open,NULL);
  (void) pthread_mutex_init(&LOCK_thread_count,NULL);
  (void) pthread_mutex_init(&LOCK_mapped_file,NULL);
  (void) pthread_mutex_init(&LOCK_status,NULL);
  (void) pthread_mutex_init(&LOCK_error_log,NULL);
  (void) pthread_mutex_init(&LOCK_delayed_insert,NULL);
  (void) pthread_mutex_init(&LOCK_delayed_status,NULL);
  (void) pthread_mutex_init(&LOCK_delayed_create,NULL);
  (void) pthread_cond_init(&COND_refresh,NULL);
  (void) pthread_cond_init(&COND_thread_cache,NULL);
  (void) pthread_cond_init(&COND_flush_thread_cache,NULL);
  (void) pthread_cond_init(&COND_manager,NULL);
  (void) pthread_mutex_init(&LOCK_manager,NULL);
  (void) pthread_mutex_init(&LOCK_crypt,NULL);
  (void) pthread_mutex_init(&LOCK_bytes_sent,NULL);
  (void) pthread_mutex_init(&LOCK_bytes_received,NULL);
  (void) pthread_mutex_init(&LOCK_timezone,NULL);
  (void) pthread_mutex_init(&LOCK_binlog_update, NULL);
  (void) pthread_mutex_init(&LOCK_slave, NULL);
  (void) pthread_mutex_init(&LOCK_server_id, NULL);
  (void) pthread_cond_init(&COND_binlog_update, NULL);
  (void) pthread_cond_init(&COND_slave_stopped, NULL);
  (void) pthread_cond_init(&COND_slave_start, NULL);

  if (set_default_charset_by_name(default_charset, MYF(MY_WME)))
    unireg_abort(1);
  charsets_list = list_charsets(MYF(MY_COMPILED_SETS|MY_CONFIG_SETS));

#ifdef HAVE_OPENSSL
  if (opt_use_ssl)
  {
    ssl_acceptor_fd = VioSSLAcceptorFd_new(opt_ssl_key, opt_ssl_cert,
					   opt_ssl_ca, opt_ssl_capath);
    if (!ssl_acceptor_fd)
      opt_use_ssl=0;
    /* having ssl_acceptor_fd!=0 signals the use of SSL */
  }
#endif /* HAVE_OPENSSL */

#ifdef HAVE_LIBWRAP
  libwrapName= my_progname+dirname_length(my_progname);
  openlog(libwrapName, LOG_PID, LOG_AUTH);
#endif

  if (!(opt_specialflag & SPECIAL_NO_PRIOR))
    my_pthread_setprio(pthread_self(),CONNECT_PRIOR);
  /* Parameter for threads created for connections */
  (void) pthread_attr_init(&connection_attrib);
  (void) pthread_attr_setdetachstate(&connection_attrib,
				     PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&connection_attrib,thread_stack);

  if (!(opt_specialflag & SPECIAL_NO_PRIOR))
    my_pthread_attr_setprio(&connection_attrib,WAIT_PRIOR);
  pthread_attr_setscope(&connection_attrib, PTHREAD_SCOPE_SYSTEM);

#ifdef SET_RLIMIT_NOFILE
  /* connections and databases neads lots of files */
  {
    uint wanted_files=10+(uint) max(max_connections*5,
				    max_connections+table_cache_size*2);
    set_if_bigger(wanted_files, open_files_limit);
    // Note that some system returns 0 if we succeed here:
    uint files=set_maximum_open_files(wanted_files);
    if (files && files < wanted_files && ! open_files_limit)
    {
      max_connections=	(ulong) min((files-10),max_connections);
      table_cache_size= (ulong) max((files-10-max_connections)/2,64);
      DBUG_PRINT("warning",
		 ("Changed limits: max_connections: %ld  table_cache: %ld",
		  max_connections,table_cache_size));
      sql_print_error("Warning: Changed limits: max_connections: %ld  table_cache: %ld",max_connections,table_cache_size);
    }
  }
#endif
  unireg_init(opt_specialflag); /* Set up extern variabels */
  init_errmessage();		/* Read error messages from file */
  lex_init();
  item_init();
  mysys_uses_curses=0;
#ifdef USE_REGEX
  regex_init();
#endif
  select_thread=pthread_self();
  select_thread_in_use=1;

  /*
  ** We have enough space for fiddling with the argv, continue
  */
  umask(((~my_umask) & 0666));
  if (my_setwd(mysql_real_data_home,MYF(MY_WME)))
  {
    unireg_abort(1);				/* purecov: inspected */
  }
  mysql_data_home[0]=FN_CURLIB;		// all paths are relative from here
  mysql_data_home[1]=0;
  server_init();
  table_cache_init();
  hostname_cache_init();
  sql_cache_init();
  randominit(&sql_rand,(ulong) start_time,(ulong) start_time/2);
  reset_floating_point_exceptions();
  init_thr_lock();

  /* Setup log files */
  if (opt_log)
    open_log(&mysql_log, glob_hostname, opt_logname, ".log", LOG_NORMAL);
  if (opt_update_log)
    open_log(&mysql_update_log, glob_hostname, opt_update_logname, "",
	     LOG_NEW);

  if (opt_bin_log && !server_id)
  {
    server_id= !master_host ? 1 : 2;
    switch (server_id) {
#ifdef EXTRA_DEBUG
    case 1:
      sql_print_error("\
Warning: one should set server-id to a non-0 value if log-bin is enabled.\n\
Will log updates to binary log, but will not accept connections from slaves.");
      break;
#endif
    case 2:
      sql_print_error("\
Warning: one should set server-id to a non-0 value if master_host is set.\n\
The server will not act as a slave.");
      break;
    }
  }
  if (opt_bin_log)
  {
    if (!opt_bin_logname)
    {
      char tmp[FN_REFLEN];
      strnmov(tmp,glob_hostname,FN_REFLEN-5);
      strmov(strcend(tmp,'.'),"-bin");
      opt_bin_logname=my_strdup(tmp,MYF(MY_WME));
    }
    mysql_bin_log.set_index_file_name(opt_binlog_index_name);
    open_log(&mysql_bin_log, glob_hostname, opt_bin_logname, "-bin",
	     LOG_BIN);
  }
  
  if (opt_slow_log)
    open_log(&mysql_slow_log, glob_hostname, opt_slow_logname, "-slow.log",
	     LOG_NORMAL);
  if (ha_init())
  {
    sql_print_error("Can't init databases");
    exit(1);
  }
#if defined(HAVE_MLOCKALL) && defined(MCL_CURRENT)
  if (locked_in_memory && !geteuid())
  {
    ha_key_cache();
    if (mlockall(MCL_CURRENT))
    {
      sql_print_error("Warning: Failed to lock memory. Errno: %d\n",errno);
    }
    else
      locked_in_memory=1;
  }
#else
  locked_in_memory=0;
#endif    

  if (opt_myisam_log)
    (void) mi_log( 1 );
  ft_init_stopwords(ft_precompiled_stopwords);       /* SerG */

#ifdef __WIN__
#define MYSQL_ERR_FILE "mysql.err"
  if (!opt_console)
  {
    freopen(MYSQL_ERR_FILE,"a+",stdout);
    freopen(MYSQL_ERR_FILE,"a+",stderr);
    FreeConsole();				// Remove window
  }
#endif

  /*
    init signals & alarm
    After this we can't quit by a simple unireg_abort
  */
  error_handler_hook = my_message_sql;
  if (pthread_key_create(&THR_THD,NULL) || pthread_key_create(&THR_NET,NULL) ||
      pthread_key_create(&THR_MALLOC,NULL))
  {
    sql_print_error("Can't create thread-keys");
    exit(1);
  }
  init_signals();				// Creates pidfile
  if (acl_init(opt_noacl))
  {
    select_thread_in_use=0;
    (void) pthread_kill(signal_thread,MYSQL_KILL_SIGNAL);
#ifndef __WIN__
    if (!opt_bootstrap)
      (void) my_delete(pidfile_name,MYF(MY_WME));	// Not neaded anymore
#endif
    exit(1);
  }
  if (!opt_noacl)
    (void) grant_init();

#ifdef HAVE_DLOPEN
  if (!opt_noacl)
    udf_init();
#endif

  if (opt_bootstrap)
  {
    int error=bootstrap(stdin);
    end_thr_alarm();				// Don't allow alarms
    unireg_abort(error ? 1 : 0);
  }
  if (opt_init_file)
  {
    if (read_init_file(opt_init_file))
    {
      end_thr_alarm();				// Don't allow alarms
      unireg_abort(1);
    }
  }
  (void) thr_setconcurrency(concurrency);	// 10 by default
#ifdef __WIN__			        //IRENA
  {
    hEventShutdown=CreateEvent(0, FALSE, FALSE, "MySqlShutdown");
    pthread_t hThread;
    if (pthread_create(&hThread,&connection_attrib,handle_shutdown,0))
      sql_print_error("Warning: Can't create thread to handle shutdown requests");

    // On "Stop Service" we have to do regular shutdown
    Service.SetShutdownEvent(hEventShutdown);
  }
#endif

  if (
#ifdef HAVE_BERKELEY_DB
      !berkeley_skip ||
#endif
      (flush_time && flush_time != ~(ulong) 0L))
  {
    pthread_t hThread;
    if (pthread_create(&hThread,&connection_attrib,handle_manager,0))
      sql_print_error("Warning: Can't create thread to manage maintenance");
  }

  // slave thread
  if (master_host)
  {
    pthread_t hThread;
    if (!opt_skip_slave_start &&
       pthread_create(&hThread, &connection_attrib, handle_slave, 0))
      sql_print_error("Warning: Can't create thread to handle slave");
    else if(opt_skip_slave_start)
      init_master_info(&glob_mi);
  }

  printf(ER(ER_READY),my_progname,server_version,"");
  fflush(stdout);

#ifdef __NT__
  if (hPipe == INVALID_HANDLE_VALUE && !have_tcpip)
  {
    sql_print_error("TCP/IP must be installed on Win98 platforms");
  }
  else
  {
    pthread_mutex_lock(&LOCK_thread_count);
    (void) pthread_cond_init(&COND_handler_count,NULL);
    {
      pthread_t hThread;
      handler_count=0;
      if ( hPipe != INVALID_HANDLE_VALUE )
      {
	handler_count++;
	if (pthread_create(&hThread,&connection_attrib,
			   handle_connections_namedpipes, 0))
	{
	  sql_print_error("Warning: Can't create thread to handle named pipes");
	  handler_count--;
	}
      }
      if (have_tcpip && !opt_disable_networking)
      {
	handler_count++;
	if (pthread_create(&hThread,&connection_attrib,
			   handle_connections_sockets, 0))
	{
	  sql_print_error("Warning: Can't create thread to handle named pipes");
	  handler_count--;
	}
      }
      while (handler_count > 0)
      {
	pthread_cond_wait(&COND_handler_count,&LOCK_thread_count);
      }
    }
    pthread_mutex_unlock(&LOCK_thread_count);
  }
#else
  handle_connections_sockets(0);
#ifdef EXTRA_DEBUG
  sql_print_error("Exiting main thread");
#endif
#endif /* __NT__ */

  /* (void) pthread_attr_destroy(&connection_attrib); */

  DBUG_PRINT("quit",("Exiting main thread"));

#ifndef __WIN__
#ifdef EXTRA_DEBUG
  sql_print_error("Before Lock_thread_count");
#endif
  (void) pthread_mutex_lock(&LOCK_thread_count);
  select_thread_in_use=0;			// For close_connections
  (void) pthread_cond_broadcast(&COND_thread_count);
  (void) pthread_mutex_unlock(&LOCK_thread_count);
#ifdef EXTRA_DEBUG
  sql_print_error("After lock_thread_count");
#endif
#else
  // remove the event, because it will not be valid anymore
  Service.SetShutdownEvent(0);
  if(hEventShutdown) CloseHandle(hEventShutdown);
  // if it was started as service on NT try to stop the service
  if(Service.IsNT())
     Service.Stop();
#endif

  /* Wait until cleanup is done */
  (void) pthread_mutex_lock(&LOCK_thread_count);
  while (!ready_to_exit)
  {
    pthread_cond_wait(&COND_thread_count,&LOCK_thread_count);
  }
  (void) pthread_mutex_unlock(&LOCK_thread_count);
  my_end(opt_endinfo ? MY_CHECK_ERROR | MY_GIVE_INFO : 0);
  exit(0);
  return(0);					/* purecov: deadcode */
}


#ifdef __WIN__
/* ------------------------------------------------------------------------
   main and thread entry function for Win32
   (all this is needed only to run mysqld as a service on WinNT)
 -------------------------------------------------------------------------- */
int mysql_service(void *p)
{
  win_main(Service.my_argc, Service.my_argv);
  return 0;
}

int main(int argc, char **argv)
{
  // check  environment variable OS
  if (Service.GetOS())	// "OS" defined; Should be NT
  {
    if (argc == 2)
    {
      if (!strcmp(argv[1],"-install") || !strcmp(argv[1],"--install"))
      {
	char path[FN_REFLEN];
	my_path(path, argv[0], "");		   // Find name in path
	fn_format(path,argv[0],path,"",1+4+16);    // Force use of full path
	if (!Service.Install(MYSQL_SERVICENAME,MYSQL_SERVICENAME,path))
	  MessageBox(NULL,"Failed to install Service",MYSQL_SERVICENAME,
		     MB_OK|MB_ICONSTOP);
	return 0;
      }
      else if (!strcmp(argv[1],"-remove") || !strcmp(argv[1],"--remove"))
      {
	Service.Remove(MYSQL_SERVICENAME);
	return 0;
      }
    }
    else if (argc == 1)		   // No arguments; start as a service
    {
      // init service
      long tmp=Service.Init(MYSQL_SERVICENAME,mysql_service);
      return 0;
    }
  }

  // This is a WIN95 machine or a start of mysqld as a standalone program
  // we have to pass the arguments, in case of NT-service this will be done
  // by ServiceMain()

  Service.my_argc=argc;
  Service.my_argv=argv;
  mysql_service(NULL);
  return 0;
}
/* ------------------------------------------------------------------------ */
#endif


static int bootstrap(FILE *file)
{
  THD *thd= new THD;
  int error;
  thd->bootstrap=1;
  thd->client_capabilities=0;
  my_net_init(&thd->net,(Vio*) 0);
  thd->max_packet_length=thd->net.max_packet;
  thd->master_access= ~0;
  thd->thread_id=thread_id++;
  thread_count++;

  bootstrap_file=file;
  if (pthread_create(&thd->real_id,&connection_attrib,handle_bootstrap,
		     (void*) thd))
  {
    sql_print_error("Warning: Can't create thread to handle bootstrap");    
    return -1;
  }
  /* Wait for thread to die */
  (void) pthread_mutex_lock(&LOCK_thread_count);
  while (thread_count)
  {
    (void) pthread_cond_wait(&COND_thread_count,&LOCK_thread_count);
    DBUG_PRINT("quit",("One thread died (count=%u)",thread_count));
  }
  (void) pthread_mutex_unlock(&LOCK_thread_count);
  error= thd->fatal_error;
  net_end(&thd->net);
  delete thd;
  return error;
}

static bool read_init_file(char *file_name)
{
  FILE *file;
  DBUG_ENTER("read_init_file");
  DBUG_PRINT("enter",("name: %s",file_name));
  if (!(file=my_fopen(file_name,O_RDONLY,MYF(MY_WME))))
    return(1);
  bootstrap(file);				/* Ignore errors from this */
  (void) my_fclose(file,MYF(MY_WME));
  return 0;
}


static void create_new_thread(THD *thd)
{
  DBUG_ENTER("create_new_thread");

  NET *net=&thd->net;				// For easy ref
  net->timeout = (uint) connect_timeout;	// Timeout for read
  if (protocol_version > 9)
    net->return_errno=1;

  /* don't allow too many connections */
  if (thread_count - delayed_insert_threads >= max_connections+1 || abort_loop)
  {
    DBUG_PRINT("error",("too many connections"));
    close_connection(net,ER_CON_COUNT_ERROR);
    delete thd;
    DBUG_VOID_RETURN;
  }
  if (pthread_mutex_lock(&LOCK_thread_count))
  {
    DBUG_PRINT("error",("Can't lock LOCK_thread_count"));
    close_connection(net,ER_OUT_OF_RESOURCES);
    delete thd;
    DBUG_VOID_RETURN;
  }
  if (thread_count-delayed_insert_threads > max_used_connections)
    max_used_connections=thread_count-delayed_insert_threads;
  thd->thread_id=thread_id++;
  for (uint i=0; i < 8 ; i++)			// Generate password teststring
    thd->scramble[i]= (char) (rnd(&sql_rand)*94+33);
  thd->scramble[8]=0;
  thd->rand=sql_rand;
  thd->real_id=pthread_self();			// Keep purify happy

  /* Start a new thread to handle connection */
#ifdef ONE_THREAD
  if (test_flags & TEST_NO_THREADS)		// For debugging under Linux
  {
    thread_cache_size=0;			// Safety
    thread_count++;
    threads.append(thd);
    thd->real_id=pthread_self();
    (void) pthread_mutex_unlock(&LOCK_thread_count);
    handle_one_connection((void*) thd);
  }
  else
#endif
  {
    if (cached_thread_count > wake_thread)
    {
      start_cached_thread(thd);
      (void) pthread_mutex_unlock(&LOCK_thread_count);
    }
    else
    {
      int error;
      thread_count++;
      thread_created++;
      threads.append(thd);
      DBUG_PRINT("info",(("creating thread %d"), thd->thread_id));
      thd->connect_time = time(NULL);
      (void) pthread_mutex_unlock(&LOCK_thread_count);
      if ((error=pthread_create(&thd->real_id,&connection_attrib,
				handle_one_connection,
				(void*) thd)))
      {
	DBUG_PRINT("error",
		   ("Can't create thread to handle request (error %d)",
		    error));
	(void) pthread_mutex_lock(&LOCK_thread_count);
	thread_count--;
	thd->killed=1;				// Safety
	(void) pthread_mutex_unlock(&LOCK_thread_count);
	net_printf(net,ER_CANT_CREATE_THREAD,error);
	(void) pthread_mutex_lock(&LOCK_thread_count);
	close_connection(net,0,0);
	delete thd;
	(void) pthread_mutex_unlock(&LOCK_thread_count);
	DBUG_VOID_RETURN;
      }
    }
  }
  DBUG_PRINT("info",(("Thread %d created"), thd->thread_id));
  DBUG_VOID_RETURN;
}


	/* Handle new connections and spawn new process to handle them */

pthread_handler_decl(handle_connections_sockets,arg __attribute__((unused)))
{
  my_socket sock,new_sock;
  uint error_count=0;
  uint max_used_connection= (uint) (max(ip_sock,unix_sock)+1);
  fd_set readFDs,clientFDs;
  THD *thd;
  struct sockaddr_in cAddr;
  int ip_flags=0,socket_flags=0,flags;
  Vio *vio_tmp;
  DBUG_ENTER("handle_connections_sockets");

  LINT_INIT(new_sock);

  (void) my_pthread_getprio(pthread_self());		// For debugging

  FD_ZERO(&clientFDs);
  if (ip_sock != INVALID_SOCKET)
  {
    FD_SET(ip_sock,&clientFDs);
#ifdef HAVE_FCNTL
    ip_flags = fcntl(ip_sock, F_GETFL, 0);
#endif
  }
#ifdef HAVE_SYS_UN_H
  FD_SET(unix_sock,&clientFDs);
  socket_flags=fcntl(unix_sock, F_GETFL, 0);
#endif

  DBUG_PRINT("general",("Waiting for connections."));
  while (!abort_loop)
  {
    readFDs=clientFDs;
#ifdef HPUX
    if (select(max_used_connection,(int*) &readFDs,0,0,0) < 0)
      continue;
#else
    if (select((int) max_used_connection,&readFDs,0,0,0) < 0)
    {
      if (errno != EINTR)
      {
	if (!select_errors++ && !abort_loop)	/* purecov: inspected */
	  sql_print_error("mysqld: Got error %d from select",errno); /* purecov: inspected */
      }
      continue;
    }
#endif	/* HPUX */
    if (abort_loop)
      break;

    /*
    ** Is this a new connection request
    */

#ifdef HAVE_SYS_UN_H
    if (FD_ISSET(unix_sock,&readFDs))
    {
      sock = unix_sock;
      flags= socket_flags;
    }
    else
#endif
    {
      sock = ip_sock;
      flags= ip_flags;
    }

#if !defined(NO_FCNTL_NONBLOCK)
    if (!(test_flags & TEST_BLOCKING))
    {
#if defined(O_NONBLOCK)
      fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#elif defined(O_NDELAY)
      fcntl(sock, F_SETFL, flags | O_NDELAY);
#endif
    }
#endif /* NO_FCNTL_NONBLOCK */
    for (uint retry=0; retry < MAX_ACCEPT_RETRY; retry++)
    {
      size_socket length=sizeof(struct sockaddr_in);
      new_sock = accept(sock, my_reinterpret_cast(struct sockaddr *) (&cAddr),
			&length);
      if (new_sock != INVALID_SOCKET || (errno != EINTR && errno != EAGAIN))
	break;
#if !defined(NO_FCNTL_NONBLOCK)
      if (!(test_flags & TEST_BLOCKING))
      {
	if (retry == MAX_ACCEPT_RETRY - 1)
	  fcntl(sock, F_SETFL, flags);		// Try without O_NONBLOCK
      }
#endif
    }
#if !defined(NO_FCNTL_NONBLOCK)
    if (!(test_flags & TEST_BLOCKING))
      fcntl(sock, F_SETFL, flags);
#endif
    if (new_sock < 0)
    {
      if ((error_count++ & 255) == 0)		// This can happen often
	sql_perror("Error in accept");
      if (errno == ENFILE || errno == EMFILE)
	sleep(1);				// Give other threads some time
      continue;
    }

#ifdef HAVE_LIBWRAP
    {
      if (sock == ip_sock)
      {
	struct request_info req;
	signal(SIGCHLD, SIG_DFL);
	request_init(&req, RQ_DAEMON, libwrapName, RQ_FILE, new_sock, NULL);
	fromhost(&req);
	if (!hosts_access(&req))
	{
	  // This may be stupid but refuse() includes an exit(0)
	  // which we surely don't want...
	  // clean_exit() - same stupid thing ...
	  syslog(deny_severity, "refused connect from %s", eval_client(&req));
	  if (req.sink)
	    ((void (*)(int))req.sink)(req.fd);

	  // C++ sucks (the gibberish in front just translates the supplied
	  // sink function pointer in the req structure from a void (*sink)();
	  // to a void(*sink)(int) if you omit the cast, the C++ compiler
	  // will cry...

	  (void) shutdown(new_sock,2);  // This looks fine to me...
	  (void) closesocket(new_sock);
	  continue;
	}
      }
    }
#endif /* HAVE_LIBWRAP */

    {
      size_socket dummyLen;
      struct sockaddr dummy;
      dummyLen = sizeof(struct sockaddr);
      if (getsockname(new_sock,&dummy, &dummyLen) < 0)
      {
	sql_perror("Error on new connection socket");
	(void) shutdown(new_sock,2);
	(void) closesocket(new_sock);
	continue;
      }
    }

    /*
    ** Don't allow too many connections
    */

    if (!(thd= new THD))
    {
      (void) shutdown(new_sock,2); VOID(closesocket(new_sock));
      continue;
    }
    if (!(vio_tmp=vio_new(new_sock,
			  sock == unix_sock ? VIO_TYPE_SOCKET :
			  VIO_TYPE_TCPIP,
			  sock == unix_sock)) ||
	my_net_init(&thd->net,vio_tmp))
    {
      if (vio_tmp)
	vio_delete(vio_tmp);
      else
      {
	(void) shutdown(new_sock,2);
	(void) closesocket(new_sock);
      }
      delete thd;
      continue;
    }
    if (sock == unix_sock)
      thd->host=(char*) localhost;
    create_new_thread(thd);
  }

#ifdef __NT__
  pthread_mutex_lock(&LOCK_thread_count);
  handler_count--;
  pthread_mutex_unlock(&LOCK_thread_count);
  pthread_cond_signal(&COND_handler_count);
#endif
  DBUG_RETURN(0);
}


#ifdef __NT__
pthread_handler_decl(handle_connections_namedpipes,arg)
{
  HANDLE hConnectedPipe;
  BOOL fConnected;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("handle_connections_namedpipes");
  (void) my_pthread_getprio(pthread_self());		// For debugging

  DBUG_PRINT("general",("Waiting for named pipe connections."));
  while (!abort_loop)
  {
    /* wait for named pipe connection */
    fConnected = ConnectNamedPipe( hPipe, NULL );
    if (abort_loop)
      break;
    if ( !fConnected )
      fConnected = GetLastError() == ERROR_PIPE_CONNECTED;
    if ( !fConnected )
    {
      CloseHandle( hPipe );
      if ((hPipe = CreateNamedPipe(szPipeName,
				   PIPE_ACCESS_DUPLEX,
				   PIPE_TYPE_BYTE |
				   PIPE_READMODE_BYTE |
				   PIPE_WAIT,
				   PIPE_UNLIMITED_INSTANCES,
				   (int) net_buffer_length,
				   (int) net_buffer_length,
				   NMPWAIT_USE_DEFAULT_WAIT,
				   &saPipeSecurity )) ==
	  INVALID_HANDLE_VALUE )
      {
	sql_perror("Can't create new named pipe!");
	break;					// Abort
      }
    }
    hConnectedPipe = hPipe;
    /* create new pipe for new connection */
    if ((hPipe = CreateNamedPipe(szPipeName,
				 PIPE_ACCESS_DUPLEX,
				 PIPE_TYPE_BYTE |
				 PIPE_READMODE_BYTE |
				 PIPE_WAIT,
				 PIPE_UNLIMITED_INSTANCES,
				 (int) net_buffer_length,
				 (int) net_buffer_length,
				 NMPWAIT_USE_DEFAULT_WAIT,
				 &saPipeSecurity)) ==
	INVALID_HANDLE_VALUE)
    {
      sql_perror("Can't create new named pipe!");
      hPipe=hConnectedPipe;
      continue;					// We have to try again
    }

    if ( !(thd = new THD))
    {
      DisconnectNamedPipe( hConnectedPipe );
      CloseHandle( hConnectedPipe );
      continue;
    }
    if (!(thd->net.vio = vio_new_win32pipe(hConnectedPipe)) ||
	my_net_init(&thd->net, thd->net.vio))
    {
      close_connection(&thd->net,ER_OUT_OF_RESOURCES);
      delete thd;
      continue;
    }
    /* host name is unknown */
    thd->host = my_strdup("localhost",MYF(0)); /* Host is unknown */
    create_new_thread(thd);
  }

  pthread_mutex_lock(&LOCK_thread_count);
  handler_count--;
  pthread_cond_signal(&COND_handler_count);
  pthread_mutex_unlock(&LOCK_thread_count);
  DBUG_RETURN(0);
}
#endif /* __NT__ */


/******************************************************************************
** handle start options
******************************************************************************/

enum options {
               OPT_ISAM_LOG=256,         OPT_SKIP_NEW, 
               OPT_SKIP_GRANT,           OPT_SKIP_LOCK, 
               OPT_ENABLE_LOCK,          OPT_USE_LOCKING,
               OPT_SOCKET,               OPT_UPDATE_LOG, 
               OPT_BIN_LOG,              OPT_SKIP_RESOLVE, 
               OPT_SKIP_NETWORKING,      OPT_BIN_LOG_INDEX,
               OPT_BIND_ADDRESS,         OPT_PID_FILE,
               OPT_SKIP_PRIOR,           OPT_BIG_TABLES,    
               OPT_STANDALONE,           OPT_ONE_THREAD,
               OPT_CONSOLE,              OPT_LOW_PRIORITY_UPDATES,
               OPT_SKIP_HOST_CACHE,      OPT_LONG_FORMAT,   
               OPT_FLUSH,                OPT_SAFE, 
               OPT_BOOTSTRAP,            OPT_SKIP_SHOW_DB,
               OPT_TABLE_TYPE,           OPT_INIT_FILE,   
               OPT_DELAY_KEY_WRITE,      OPT_SLOW_QUERY_LOG, 
               OPT_SKIP_DELAY_KEY_WRITE, OPT_CHARSETS_DIR,
               OPT_BDB_HOME,             OPT_BDB_LOG,  
               OPT_BDB_TMP,              OPT_BDB_NOSYNC,
               OPT_BDB_LOCK,             OPT_BDB_SKIP, 
               OPT_BDB_NO_RECOVER,	 OPT_BDB_SHARED,
	       OPT_MASTER_HOST,  
               OPT_MASTER_USER,          OPT_MASTER_PASSWORD,
               OPT_MASTER_PORT,          OPT_MASTER_INFO_FILE,
               OPT_MASTER_CONNECT_RETRY, OPT_SQL_BIN_UPDATE_SAME,
               OPT_REPLICATE_DO_DB,      OPT_REPLICATE_IGNORE_DB, 
               OPT_LOG_SLAVE_UPDATES,    OPT_BINLOG_DO_DB, 
               OPT_BINLOG_IGNORE_DB,     OPT_WANT_CORE,
	       OPT_SKIP_CONCURRENT_INSERT, OPT_MEMLOCK, OPT_MYISAM_RECOVER,
	       OPT_REPLICATE_REWRITE_DB, OPT_SERVER_ID, OPT_SKIP_SLAVE_START,
	       OPT_SKIP_INNOBASE,OPT_SAFEMALLOC_MEM_LIMIT,
	       OPT_REPLICATE_DO_TABLE, OPT_REPLICATE_IGNORE_TABLE,
	       OPT_REPLICATE_WILD_DO_TABLE, OPT_REPLICATE_WILD_IGNORE_TABLE,
	       OPT_DISCONNECT_SLAVE_EVENT_COUNT, OPT_ABORT_SLAVE_EVENT_COUNT,
	       OPT_INNOBASE_DATA_HOME_DIR,OPT_INNOBASE_DATA_FILE_PATH,
	       OPT_INNOBASE_LOG_GROUP_HOME_DIR,
	       OPT_INNOBASE_LOG_ARCH_DIR, OPT_INNOBASE_LOG_ARCHIVE,
	       OPT_INNOBASE_FLUSH_LOG_AT_TRX_COMMIT, OPT_SAFE_SHOW_DB,
	       OPT_GEMINI_SKIP
};

static struct option long_options[] = {
  {"ansi",                  no_argument,       0, 'a'},
  {"basedir",               required_argument, 0, 'b'},
#ifdef HAVE_BERKELEY_DB
  {"bdb-home",              required_argument, 0, (int) OPT_BDB_HOME},
  {"bdb-lock-detect",       required_argument, 0, (int) OPT_BDB_LOCK},
  {"bdb-logdir",            required_argument, 0, (int) OPT_BDB_LOG},
  {"bdb-no-recover",        no_argument,       0, (int) OPT_BDB_NO_RECOVER},
  {"bdb-no-sync",           no_argument,       0, (int) OPT_BDB_NOSYNC},
  {"bdb-shared-data",       no_argument,       0, (int) OPT_BDB_SHARED},
  {"bdb-tmpdir",            required_argument, 0, (int) OPT_BDB_TMP},
#endif
  {"big-tables",            no_argument,       0, (int) OPT_BIG_TABLES},
  {"binlog-do-db",          required_argument, 0, (int) OPT_BINLOG_DO_DB},
  {"binlog-ignore-db",      required_argument, 0, (int) OPT_BINLOG_IGNORE_DB},
  {"bind-address",          required_argument, 0, (int) OPT_BIND_ADDRESS},
  {"bootstrap",             no_argument,       0, (int) OPT_BOOTSTRAP},
#ifdef __WIN__
  {"console",               no_argument,       0, (int) OPT_CONSOLE},
#endif
  {"core-file",             no_argument,       0, (int) OPT_WANT_CORE},
  {"chroot",                required_argument, 0, 'r'},
  {"character-sets-dir",    required_argument, 0, (int) OPT_CHARSETS_DIR},
  {"datadir",               required_argument, 0, 'h'},
#ifndef DBUG_OFF
  {"debug",                 optional_argument, 0, '#'},
#endif
  {"default-character-set", required_argument, 0, 'C'},
  {"default-table-type",    required_argument, 0, (int) OPT_TABLE_TYPE},
  {"delay-key-write-for-all-tables",
                            no_argument,       0, (int) OPT_DELAY_KEY_WRITE},
  {"enable-locking",        no_argument,       0, (int) OPT_ENABLE_LOCK},
  {"exit-info",             optional_argument, 0, 'T'},
  {"flush",                 no_argument,       0, (int) OPT_FLUSH},
#ifdef HAVE_INNOBASE_DB
  {"innobase_data_home_dir", required_argument, 0,
     OPT_INNOBASE_DATA_HOME_DIR},
  {"innobase_data_file_path", required_argument, 0, 
     OPT_INNOBASE_DATA_FILE_PATH},
  {"innobase_log_group_home_dir", required_argument, 0,
    OPT_INNOBASE_LOG_GROUP_HOME_DIR},
  {"innobase_log_arch_dir", required_argument, 0,
    OPT_INNOBASE_LOG_ARCH_DIR},
  {"innobase_log_archive", optional_argument, 0,
     OPT_INNOBASE_LOG_ARCHIVE},
  {"innobase_flush_log_at_trx_commit", optional_argument, 0,
     OPT_INNOBASE_FLUSH_LOG_AT_TRX_COMMIT},
#endif
  {"help",                  no_argument,       0, '?'},
  {"init-file",             required_argument, 0, (int) OPT_INIT_FILE},
  {"log",                   optional_argument, 0, 'l'},
  {"language",              required_argument, 0, 'L'},
  {"log-bin",               optional_argument, 0, (int) OPT_BIN_LOG},
  {"log-bin-index",         required_argument, 0, (int) OPT_BIN_LOG_INDEX},
  {"log-isam",              optional_argument, 0, (int) OPT_ISAM_LOG},
  {"log-update",            optional_argument, 0, (int) OPT_UPDATE_LOG},
  {"log-slow-queries",      optional_argument, 0, (int) OPT_SLOW_QUERY_LOG},
  {"log-long-format",       no_argument,       0, (int) OPT_LONG_FORMAT},
  {"log-slave-updates",     no_argument,       0, (int) OPT_LOG_SLAVE_UPDATES},
  {"low-priority-updates",  no_argument,       0, (int) OPT_LOW_PRIORITY_UPDATES},
  {"master-host",           required_argument, 0, (int) OPT_MASTER_HOST},
  {"master-user",           required_argument, 0, (int) OPT_MASTER_USER},
  {"master-password",       required_argument, 0, (int) OPT_MASTER_PASSWORD},
  {"master-port",           required_argument, 0, (int) OPT_MASTER_PORT},
  {"master-connect-retry",  required_argument, 0, (int) OPT_MASTER_CONNECT_RETRY},
  {"master-info-file",      required_argument, 0, (int) OPT_MASTER_INFO_FILE},
  {"myisam-recover",	    optional_argument, 0, (int) OPT_MYISAM_RECOVER},
  {"memlock",		    no_argument,       0, (int) OPT_MEMLOCK},
    // needs to be available for the test case to pass in non-debugging mode
    // is a no-op
  {"disconnect-slave-event-count",      required_argument, 0,
     (int) OPT_DISCONNECT_SLAVE_EVENT_COUNT},
  {"abort-slave-event-count",      required_argument, 0,
     (int) OPT_ABORT_SLAVE_EVENT_COUNT},
#if !defined(DBUG_OFF) && defined(SAFEMALLOC)
  {"safemalloc-mem-limit",  required_argument, 0, (int)
     OPT_SAFEMALLOC_MEM_LIMIT},
#endif    
  {"new",                   no_argument,       0, 'n'},
  {"old-protocol",          no_argument,       0, 'o'},
#ifdef ONE_THREAD
  {"one-thread",            no_argument,       0, (int) OPT_ONE_THREAD},
#endif
  {"pid-file",              required_argument, 0, (int) OPT_PID_FILE},
  {"port",                  required_argument, 0, 'P'},
  {"replicate-do-db",       required_argument, 0, (int) OPT_REPLICATE_DO_DB},
  {"replicate-do-table",       required_argument, 0,
   (int) OPT_REPLICATE_DO_TABLE},
  {"replicate-wild-do-table",       required_argument, 0,
   (int) OPT_REPLICATE_WILD_DO_TABLE},
  {"replicate-ignore-db",   required_argument, 0,
   (int) OPT_REPLICATE_IGNORE_DB},
  {"replicate-ignore-table",   required_argument, 0,
   (int) OPT_REPLICATE_IGNORE_TABLE},
  {"replicate-wild-ignore-table",   required_argument, 0,
   (int) OPT_REPLICATE_WILD_IGNORE_TABLE},
  {"replicate-rewrite-db",   required_argument, 0,
     (int) OPT_REPLICATE_REWRITE_DB},
  {"safe-mode",             no_argument,       0, (int) OPT_SAFE},
  {"safe-show-database",    no_argument,       0, (int) OPT_SAFE_SHOW_DB},
  {"socket",                required_argument, 0, (int) OPT_SOCKET},
  {"server-id",		    required_argument, 0, (int) OPT_SERVER_ID},
  {"set-variable",          required_argument, 0, 'O'},
#ifdef HAVE_BERKELEY_DB
  {"skip-bdb",              no_argument,       0, (int) OPT_BDB_SKIP},
#endif
#ifdef HAVE_INNOBASE_DB
  {"skip-innobase",         no_argument,       0, (int) OPT_INNOBASE_SKIP},
#endif
#ifdef HAVE_GEMINI_DB
  {"skip-gemini",           no_argument,       0, (int) OPT_GEMINI_SKIP},
#endif
  {"skip-concurrent-insert", no_argument,      0, (int) OPT_SKIP_CONCURRENT_INSERT},
  {"skip-delay-key-write",  no_argument,       0, (int) OPT_SKIP_DELAY_KEY_WRITE},
  {"skip-grant-tables",     no_argument,       0, (int) OPT_SKIP_GRANT},
  {"skip-locking",          no_argument,       0, (int) OPT_SKIP_LOCK},
  {"skip-host-cache",       no_argument,       0, (int) OPT_SKIP_HOST_CACHE},
  {"skip-name-resolve",     no_argument,       0, (int) OPT_SKIP_RESOLVE},
  {"skip-new",              no_argument,       0, (int) OPT_SKIP_NEW},
  {"skip-show-database",    no_argument,       0, (int) OPT_SKIP_SHOW_DB},
  {"skip-slave-start",    no_argument,       0, (int) OPT_SKIP_SLAVE_START},
  {"skip-networking",       no_argument,       0, (int) OPT_SKIP_NETWORKING},
  {"skip-thread-priority",  no_argument,       0, (int) OPT_SKIP_PRIOR},
  {"sql-bin-update-same",   no_argument,       0, (int) OPT_SQL_BIN_UPDATE_SAME},
#include "sslopt-longopts.h"
#ifdef __WIN__
  {"standalone",            no_argument,       0, (int) OPT_STANDALONE},
#endif
  {"tmpdir",                required_argument, 0, 't'},
  {"use-locking",           no_argument,       0, (int) OPT_USE_LOCKING},
#ifdef USE_SYMDIR
  {"use-symbolic-links",    no_argument,       0, 's'},
#endif
  {"user",                  required_argument, 0, 'u'},
  {"version",               no_argument,       0, 'V'},
  {0, 0, 0, 0}
};

CHANGEABLE_VAR changeable_vars[] = {
  { "back_log",                (long*) &back_log, 
      50, 1, 65535, 0, 1 },
#ifdef HAVE_BERKELEY_DB
  { "bdb_cache_size",          (long*) &berkeley_cache_size, 
      KEY_CACHE_SIZE, 20*1024, (long) ~0, 0, IO_SIZE },
  {"bdb_log_buffer_size",      (long*) &berkeley_log_buffer_size, 0, 256*1024L,
     ~0L, 0, 1024},
  { "bdb_max_lock",            (long*) &berkeley_max_lock, 
      10000, 0, (long) ~0, 0, 1 },
    /* QQ: The following should be removed soon! */
  { "bdb_lock_max",            (long*) &berkeley_max_lock, 
      10000, 0, (long) ~0, 0, 1 },
#endif
  { "binlog_cache_size", 	(long*) &binlog_cache_size,
      32*1024L, IO_SIZE, ~0L, 0, IO_SIZE },
  { "connect_timeout",         (long*) &connect_timeout,
      CONNECT_TIMEOUT, 2, 65535, 0, 1 },
  { "delayed_insert_timeout", (long*) &delayed_insert_timeout, 
      DELAYED_WAIT_TIMEOUT, 1, ~0L, 0, 1 },
  { "delayed_insert_limit",    (long*) &delayed_insert_limit, 
      DELAYED_LIMIT, 1, ~0L, 0, 1 },
  { "delayed_queue_size",      (long*) &delayed_queue_size,
      DELAYED_QUEUE_SIZE, 1, ~0L, 0, 1 },
  { "flush_time",              (long*) &flush_time,
      FLUSH_TIME, 0, ~0L, 0, 1 },
#ifdef HAVE_INNOBASE_DB
  {"innobase_mirrored_log_groups",
   (long*) &innobase_mirrored_log_groups, 1, 1, 10, 0, 1},
  {"innobase_log_files_in_group", 
     (long*) &innobase_mirrored_log_groups, 2, 2, 100, 0, 1},
  {"innobase_log_file_size", 
     (long*) &innobase_log_file_size, 5*1024*1024L, 1*1024*1024L,
     ~0L, 0, 1024*1024L},
  {"innobase_log_buffer_size", 
     (long*) &innobase_log_buffer_size, 1024*1024L, 256*1024L,
     ~0L, 0, 1024},
  {"innobase_buffer_pool_size", 
     (long*) &innobase_buffer_pool_size, 8*1024*1024L, 1024*1024L,
     ~0L, 0, 1024*1024L},
  {"innobase_additional_mem_pool_size",
   (long*) &innobase_additional_mem_pool_size, 1*1024*1024L, 512*1024L,
     ~0L, 0, 1024},
  {"innobase_file_io_threads",
     (long*) &innobase_file_io_threads, 9, 4, 64, 0, 1},
#endif
  { "interactive_timeout",     (long*) &net_interactive_timeout,
      NET_WAIT_TIMEOUT, 1, 31*24*60*60, 0, 1 },
  { "join_buffer_size",        (long*) &join_buff_size,
      128*1024L, IO_SIZE*2+MALLOC_OVERHEAD, ~0L, MALLOC_OVERHEAD, IO_SIZE },
  { "key_buffer_size",         (long*) &keybuff_size,
      KEY_CACHE_SIZE, MALLOC_OVERHEAD, (long) ~0, MALLOC_OVERHEAD, IO_SIZE },
  { "long_query_time",         (long*) &long_query_time,
      10, 1, ~0L, 0, 1 },
  { "lower_case_table_names",  (long*) &lower_case_table_names,
      IF_WIN(1,0), 0, 1, 0, 1 },
  { "max_allowed_packet",      (long*) &max_allowed_packet,
      1024*1024L, 80, 64*1024*1024L, MALLOC_OVERHEAD, 1024 },
  { "max_binlog_cache_size",   (long*) &max_binlog_cache_size,
      ~0L, IO_SIZE, ~0L, 0, IO_SIZE },
  { "max_connections",         (long*) &max_connections,
      100, 1, 16384, 0, 1 },
  { "max_connect_errors",      (long*) &max_connect_errors,
      MAX_CONNECT_ERRORS, 1, ~0L, 0, 1 },
  { "max_delayed_threads",     (long*) &max_insert_delayed_threads,
      20, 1, 16384, 0, 1 },
  { "max_heap_table_size",     (long*) &max_heap_table_size,
      16*1024*1024L, 16384, ~0L, MALLOC_OVERHEAD, 1024 },
  { "max_join_size",           (long*) &max_join_size,
      ~0L, 1, ~0L, 0, 1 },
  { "max_sort_length",         (long*) &max_item_sort_length,
      1024, 4, 8192*1024L, 0, 1 },
  { "max_tmp_tables",          (long*) &max_tmp_tables,
      32, 1, ~0L, 0, 1 },
  { "max_write_lock_count",    (long*) &max_write_lock_count,
      ~0L, 1, ~0L, 0, 1 },
  { "myisam_sort_buffer_size", (long*) &myisam_sort_buffer_size,
      8192*1024, 4, ~0L, 0, 1 },
  { "net_buffer_length",       (long*) &net_buffer_length,
      16384, 1024, 1024*1024L, MALLOC_OVERHEAD, 1024 },
  { "net_retry_count",         (long*) &mysqld_net_retry_count,
      MYSQLD_NET_RETRY_COUNT, 1, ~0L, 0, 1 },
  { "net_read_timeout",        (long*) &net_read_timeout, 
      NET_READ_TIMEOUT, 1, 65535, 0, 1 },
  { "net_write_timeout",       (long*) &net_write_timeout,
      NET_WRITE_TIMEOUT, 1, 65535, 0, 1 },
  { "open_files_limit",        (long*) &open_files_limit,
      0, 0, 65535, 0, 1},
  { "query_buffer_size",       (long*) &query_buff_size,
      0, MALLOC_OVERHEAD, (long) ~0, MALLOC_OVERHEAD, IO_SIZE },
  { "record_buffer",           (long*) &my_default_record_cache_size,
      128*1024L, IO_SIZE*2+MALLOC_OVERHEAD, ~0L, MALLOC_OVERHEAD, IO_SIZE },
  { "slow_launch_time",        (long*) &slow_launch_time, 
      2L, 0L, ~0L, 0, 1 },
  { "sort_buffer",             (long*) &sortbuff_size,
      MAX_SORT_MEMORY, MIN_SORT_MEMORY+MALLOC_OVERHEAD*2, ~0L, MALLOC_OVERHEAD, 1 },
  { "table_cache",             (long*) &table_cache_size,
      64, 1, 16384, 0, 1 },
  { "thread_concurrency",      (long*) &concurrency,
      DEFAULT_CONCURRENCY, 1, 512, 0, 1 },
  { "thread_cache_size",       (long*) &thread_cache_size,
      0, 0, 16384, 0, 1 },
  { "tmp_table_size",          (long*) &tmp_table_size,
      1024*1024L, 1024, ~0L, MALLOC_OVERHEAD, 1 },
  { "thread_stack",            (long*) &thread_stack,
      DEFAULT_THREAD_STACK, 1024*32, ~0L, 0, 1024 },
  { "wait_timeout",            (long*) &net_wait_timeout,
      NET_WAIT_TIMEOUT, 1, ~0L, 0, 1 },
  { NullS, (long*) 0, 0, 0, 0, 0, 0}
};


struct show_var_st init_vars[]= {
  {"ansi_mode",               (char*) &opt_ansi_mode,               SHOW_BOOL},
  {"back_log",                (char*) &back_log,                    SHOW_LONG},
  {"basedir",                 mysql_home,                           SHOW_CHAR},
#ifdef HAVE_BERKELEY_DB
  {"bdb_cache_size",          (char*) &berkeley_cache_size,         SHOW_LONG},
  {"bdb_log_buffer_size",     (char*) &berkeley_log_buffer_size,    SHOW_LONG},
  {"bdb_home",                (char*) &berkeley_home,               SHOW_CHAR_PTR},
  {"bdb_max_lock",            (char*) &berkeley_max_lock,	    SHOW_LONG},
  {"bdb_logdir",              (char*) &berkeley_logdir,             SHOW_CHAR_PTR},
  {"bdb_shared_data",	      (char*) &berkeley_shared_data,	    SHOW_BOOL},
  {"bdb_tmpdir",              (char*) &berkeley_tmpdir,             SHOW_CHAR_PTR},
  {"bdb_version",             (char*) DB_VERSION_STRING,            SHOW_CHAR},
#endif
  {"binlog_cache_size",       (char*) &binlog_cache_size,	    SHOW_LONG},
  {"character_set",           default_charset,                      SHOW_CHAR},
  {"character_sets",          (char*) &charsets_list,               SHOW_CHAR_PTR},
  {"concurrent_insert",       (char*) &myisam_concurrent_insert,    SHOW_MY_BOOL},
  {"connect_timeout",         (char*) &connect_timeout,             SHOW_LONG},
  {"datadir",                 mysql_real_data_home,                 SHOW_CHAR},
  {"delay_key_write",         (char*) &myisam_delay_key_write,      SHOW_MY_BOOL},
  {"delayed_insert_limit",    (char*) &delayed_insert_limit,        SHOW_LONG},
  {"delayed_insert_timeout",  (char*) &delayed_insert_timeout,      SHOW_LONG},
  {"delayed_queue_size",      (char*) &delayed_queue_size,          SHOW_LONG},
  {"flush",                   (char*) &myisam_flush,                SHOW_MY_BOOL},
  {"flush_time",              (char*) &flush_time,                  SHOW_LONG},
  {"have_bdb",		      (char*) &have_berkeley_db,	    SHOW_HAVE},
  {"have_gemini",	      (char*) &have_gemini,		    SHOW_HAVE},
  {"have_innobase",	      (char*) &have_innobase,		    SHOW_HAVE},
  {"have_isam",	      	      (char*) &have_isam,		    SHOW_HAVE},
  {"have_raid",		      (char*) &have_raid,		    SHOW_HAVE},
  {"have_ssl",		      (char*) &have_ssl,		    SHOW_HAVE},
  {"init_file",               (char*) &opt_init_file,               SHOW_CHAR_PTR},
  {"interactive_timeout",     (char*) &net_interactive_timeout,     SHOW_LONG},
  {"join_buffer_size",        (char*) &join_buff_size,              SHOW_LONG},
  {"key_buffer_size",         (char*) &keybuff_size,                SHOW_LONG},
  {"language",                language,                             SHOW_CHAR},
  {"large_files_support",     (char*) &opt_large_files,             SHOW_BOOL},	
#ifdef HAVE_MLOCKALL
  {"locked_in_memory",	      (char*) &locked_in_memory,	    SHOW_BOOL},
#endif
  {"log",                     (char*) &opt_log,                     SHOW_BOOL},
  {"log_update",              (char*) &opt_update_log,              SHOW_BOOL},
  {"log_bin",                 (char*) &opt_bin_log,                 SHOW_BOOL},
  {"log_slave_updates",       (char*) &opt_log_slave_updates,       SHOW_BOOL},
  {"long_query_time",         (char*) &long_query_time,             SHOW_LONG},
  {"low_priority_updates",    (char*) &low_priority_updates,        SHOW_BOOL},
  {"lower_case_table_names",  (char*) &lower_case_table_names,      SHOW_LONG},
  {"max_allowed_packet",      (char*) &max_allowed_packet,          SHOW_LONG},
  {"max_binlog_cache_size",  (char*) &max_binlog_cache_size,	    SHOW_LONG},
  {"max_connections",         (char*) &max_connections,             SHOW_LONG},
  {"max_connect_errors",      (char*) &max_connect_errors,          SHOW_LONG},
  {"max_delayed_threads",     (char*) &max_insert_delayed_threads,  SHOW_LONG},
  {"max_heap_table_size",     (char*) &max_heap_table_size,         SHOW_LONG},
  {"max_join_size",           (char*) &max_join_size,               SHOW_LONG},
  {"max_sort_length",         (char*) &max_item_sort_length,        SHOW_LONG},
  {"max_tmp_tables",          (char*) &max_tmp_tables,              SHOW_LONG},
  {"max_write_lock_count",    (char*) &max_write_lock_count,        SHOW_LONG},
  {"myisam_recover_options",  (char*) &myisam_recover_options_str,  SHOW_CHAR_PTR},
  {"myisam_sort_buffer_size", (char*) &myisam_sort_buffer_size,     SHOW_LONG},
  {"net_buffer_length",       (char*) &net_buffer_length,           SHOW_LONG},
  {"net_read_timeout",        (char*) &net_read_timeout,	    SHOW_LONG},
  {"net_retry_count",         (char*) &mysqld_net_retry_count,      SHOW_LONG},
  {"net_write_timeout",       (char*) &net_write_timeout,	    SHOW_LONG},
  {"open_files_limit",	      (char*) &open_files_limit,	    SHOW_LONG},
  {"pid_file",                (char*) pidfile_name,                 SHOW_CHAR},
  {"port",                    (char*) &mysql_port,                  SHOW_INT},
  {"protocol_version",        (char*) &protocol_version,            SHOW_INT},
  {"record_buffer",           (char*) &my_default_record_cache_size,SHOW_LONG},
  {"query_buffer_size",       (char*) &query_buff_size,		    SHOW_LONG},
  {"safe_show_database",      (char*) &opt_safe_show_db,            SHOW_BOOL},
  {"server_id",               (char*) &server_id,		    SHOW_LONG},
  {"skip_locking",            (char*) &my_disable_locking,          SHOW_MY_BOOL},
  {"skip_networking",         (char*) &opt_disable_networking,      SHOW_BOOL},
  {"skip_show_database",      (char*) &opt_skip_show_db,            SHOW_BOOL},
  {"slow_launch_time",        (char*) &slow_launch_time,            SHOW_LONG},
  {"socket",                  (char*) &mysql_unix_port,             SHOW_CHAR_PTR},
  {"sort_buffer",             (char*) &sortbuff_size,               SHOW_LONG},
  {"table_cache",             (char*) &table_cache_size,            SHOW_LONG},
  {"table_type",              (char*) &default_table_type_name,     SHOW_CHAR_PTR},
  {"thread_cache_size",       (char*) &thread_cache_size,           SHOW_LONG},
#ifdef HAVE_THR_SETCONCURRENCY
  {"thread_concurrency",      (char*) &concurrency,                 SHOW_LONG},
#endif
  {"thread_stack",            (char*) &thread_stack,                SHOW_LONG},
#ifdef HAVE_TZNAME
  {"timezone",                time_zone,                            SHOW_CHAR},
#endif
  {"tmp_table_size",          (char*) &tmp_table_size,              SHOW_LONG},
  {"tmpdir",                  (char*) &mysql_tmpdir,                SHOW_CHAR_PTR},
  {"version",                 server_version,                       SHOW_CHAR},
  {"wait_timeout",            (char*) &net_wait_timeout,            SHOW_LONG},
  {NullS, NullS, SHOW_LONG}
};

struct show_var_st status_vars[]= {
  {"Aborted_clients",          (char*) &aborted_threads,        SHOW_LONG},
  {"Aborted_connects",         (char*) &aborted_connects,       SHOW_LONG},
  {"Bytes_received",           (char*) &bytes_received,         SHOW_LONG},
  {"Bytes_sent",               (char*) &bytes_sent,             SHOW_LONG},
  {"Connections",              (char*) &thread_id,              SHOW_LONG_CONST},
  {"Created_tmp_disk_tables",  (char*) &created_tmp_disk_tables,SHOW_LONG},
  {"Created_tmp_tables",       (char*) &created_tmp_tables,     SHOW_LONG},
  {"Created_tmp_files",	       (char*) &my_tmp_file_created,	SHOW_LONG},
  {"Delayed_insert_threads",   (char*) &delayed_insert_threads, SHOW_LONG},
  {"Delayed_writes",           (char*) &delayed_insert_writes,  SHOW_LONG},
  {"Delayed_errors",           (char*) &delayed_insert_errors,  SHOW_LONG},
  {"Flush_commands",           (char*) &refresh_version,        SHOW_LONG_CONST},
  {"Handler_delete",           (char*) &ha_delete_count,        SHOW_LONG},
  {"Handler_read_first",       (char*) &ha_read_first_count,    SHOW_LONG},
  {"Handler_read_key",         (char*) &ha_read_key_count,      SHOW_LONG},
  {"Handler_read_next",        (char*) &ha_read_next_count,     SHOW_LONG},
  {"Handler_read_prev",        (char*) &ha_read_prev_count,     SHOW_LONG},
  {"Handler_read_rnd",         (char*) &ha_read_rnd_count,      SHOW_LONG},
  {"Handler_read_rnd_next",    (char*) &ha_read_rnd_next_count, SHOW_LONG},
  {"Handler_update",           (char*) &ha_update_count,        SHOW_LONG},
  {"Handler_write",            (char*) &ha_write_count,         SHOW_LONG},
  {"Key_blocks_used",          (char*) &_my_blocks_used,        SHOW_LONG_CONST},
  {"Key_read_requests",        (char*) &_my_cache_r_requests,   SHOW_LONG},
  {"Key_reads",                (char*) &_my_cache_read,         SHOW_LONG},
  {"Key_write_requests",       (char*) &_my_cache_w_requests,   SHOW_LONG},
  {"Key_writes",               (char*) &_my_cache_write,        SHOW_LONG},
  {"Max_used_connections",     (char*) &max_used_connections,   SHOW_LONG},
  {"Not_flushed_key_blocks",   (char*) &_my_blocks_changed,     SHOW_LONG_CONST},
  {"Not_flushed_delayed_rows", (char*) &delayed_rows_in_use,    SHOW_LONG_CONST},
  {"Open_tables",              (char*) 0,                       SHOW_OPENTABLES},
  {"Open_files",               (char*) &my_file_opened,         SHOW_INT_CONST},
  {"Open_streams",             (char*) &my_stream_opened,       SHOW_INT_CONST},
  {"Opened_tables",            (char*) &opened_tables,          SHOW_LONG},
  {"Questions",                (char*) 0,                       SHOW_QUESTION},
  {"Select_full_join",         (char*) &select_full_join_count, SHOW_LONG},
  {"Select_full_range_join",   (char*) &select_full_range_join_count, SHOW_LONG},
  {"Select_range",             (char*) &select_range_count, 	SHOW_LONG},
  {"Select_range_check",       (char*) &select_range_check_count, SHOW_LONG},
  {"Select_scan",	       (char*) &select_scan_count,	SHOW_LONG},
  {"Slave_running",            (char*) &slave_running,          SHOW_BOOL},
  {"Slave_open_temp_tables",   (char*) &slave_open_temp_tables, SHOW_LONG},
  {"Slow_launch_threads",      (char*) &slow_launch_threads,    SHOW_LONG},
  {"Slow_queries",             (char*) &long_query_count,       SHOW_LONG},
  {"Sort_merge_passes",	       (char*) &filesort_merge_passes,  SHOW_LONG},
  {"Sort_range",	       (char*) &filesort_range_count,   SHOW_LONG},
  {"Sort_rows",		       (char*) &filesort_rows,	        SHOW_LONG},
  {"Sort_scan",		       (char*) &filesort_scan_count,    SHOW_LONG},
  {"Threads_cached",           (char*) &cached_thread_count,    SHOW_LONG_CONST},
  {"Threads_created",	       (char*) &thread_created,		SHOW_LONG_CONST},
  {"Threads_connected",        (char*) &thread_count,           SHOW_INT_CONST},
  {"Threads_running",          (char*) &thread_running,         SHOW_INT_CONST},
  {"Uptime",                   (char*) 0,                       SHOW_STARTTIME},
  {NullS, NullS, SHOW_LONG}
};

static void print_version(void)
{
  printf("%s  Ver %s for %s on %s\n",my_progname,
	 server_version,SYSTEM_TYPE,MACHINE_TYPE);
}

static void use_help(void)
{
  print_version();
  printf("Use '--help' or '--no-defaults --help' for a list of available options\n");
}  

static void usage(void)
{
  print_version();
  puts("Copyright (C) 2000 MySQL AB & MySQL Finland AB, by Monty and others");
  puts("This software comes with ABSOLUTELY NO WARRANTY. This is free software,");
  puts("and you are welcome to modify and redistribute it under the GPL license\n");
  puts("Starts the MySQL server\n");

  printf("Usage: %s [OPTIONS]\n", my_progname);
  puts("\n\
  --ansi		Use ANSI SQL syntax instead of MySQL syntax\n\
  -b, --basedir=path	Path to installation directory. All paths are\n\
			usually resolved relative to this\n\
  --big-tables		Allow big result sets by saving all temporary sets\n\
			on file (Solves most 'table full' errors)\n\
  --bind-address=IP	Ip address to bind to\n\
  --bootstrap		Used by mysql installation scripts\n\
  --character-sets-dir=...\n\
                        Directory where character sets are\n\
  --chroot=path		Chroot mysqld daemon during startup\n\
  --core-file		Write core on errors\n\
  -h, --datadir=path	Path to the database root");
#ifndef DBUG_OFF
  printf("\
  -#, --debug[=...]     Debug log. Default is '%s'\n",default_dbug_option);
#endif
  puts("\
  --default-character-set=charset\n\
			Set the default character set\n\
  --default-table-type=type\n\
			Set the default table type for tables\n\
  --delay-key-write-for-all-tables\n\
			Don't flush key buffers between writes for any MyISAM\n\
			table\n\
  --enable-locking	Enable system locking\n\
  -T, --exit-info	Used for debugging;  Use at your own risk!\n\
  --flush		Flush tables to disk between SQL commands\n\
  -?, --help		Display this help and exit\n\
  --init-file=file	Read SQL commands from this file at startup\n\
  -L, --language=...	Client error messages in given language. May be\n\
			given as a full path\n\
  -l, --log[=file]	Log connections and queries to file\n\
  --log-bin[=file]      Log queries in new binary format (for replication)\n\
  --log-bin-index=file  File that holds the names for last binary log files\n\
  --log-update[=file]	Log updates to file.# where # is a unique number\n\
			if not given.\n\
  --log-isam[=file]	Log all MyISAM changes to file\n\
  --log-long-format	Log some extra information to update log\n\
  --low-priority-updates INSERT/DELETE/UPDATE has lower priority than selects\n\
  --log-slow-queries=[file]\n\
			Log slow queries to this log file.  Defaults logging\n\
                        to hostname-slow.log\n\
  --pid-file=path	Pid file used by safe_mysqld\n\
  --myisam-recover[=option[,option...]] where options is one of DEAULT,\n\
			BACKUP or FORCE.\n\
  --memlock		Lock mysqld in memory\n\
  -n, --new		Use very new possible 'unsafe' functions\n\
  -o, --old-protocol	Use the old (3.20) protocol\n\
  -P, --port=...	Port number to use for connection\n");
#ifdef ONE_THREAD
  puts("\
  --one-thread		Only use one thread (for debugging under Linux)\n");
#endif
  puts("\
  -O, --set-variable var=option\n\
			Give a variable an value. --help lists variables\n\
  -Sg, --skip-grant-tables\n\
			Start without grant tables. This gives all users\n\
			FULL ACCESS to all tables!\n\
  --safe-mode		Skip some optimize stages (for testing)\n\
  --skip-concurrent-insert\n\
		        Don't use concurrent insert with MyISAM\n\
  --skip-delay-key-write\n\
			Ignore the delay_key_write option for all tables\n\
  --skip-locking	Don't use system locking. To use isamchk one has\n\
			to shut down the server.\n\
  --skip-name-resolve	Don't resolve hostnames.\n\
			All hostnames are IP's or 'localhost'\n\
  --skip-networking	Don't allow connection with TCP/IP.\n\
  --skip-new		Don't use new, possible wrong routines.\n\
  --skip-host-cache	Don't cache host names\n");
  /* We have to break the string here because of VC++ limits */
  puts("\
  --skip-show-database  Don't allow 'SHOW DATABASE' commands\n\
  --skip-thread-priority\n\
			Don't give threads different priorities.\n\
  --socket=...		Socket file to use for connection\n\
  -t, --tmpdir=path	Path for temporary files\n\
  -u, --user=user_name	Run mysqld daemon as user\n\
  -V, --version		output version information and exit");
#ifdef __WIN__
  puts("NT and Win32 specific options:\n\
  --console		Don't remove the console window\n\
  --install		Install mysqld as a service (NT)\n\
  --remove		Remove mysqld from the service list (NT)\n\
  --standalone		Dummy option to start as a standalone program (NT)\n\
");
#endif
#ifdef HAVE_BERKELEY_DB
  puts("\
  --bdb-home=  directory  Berkeley home direcory\n\
  --bdb-lock-detect=#	  Berkeley lock detect\n\
                          (DEFAULT, OLDEST, RANDOM or YOUNGEST, # sec)\n\
  --bdb-logdir=directory  Berkeley DB log file directory\n\
  --bdb-no-sync		  Don't synchronously flush logs\n\
  --bdb-no-recover	  Don't try to recover Berkeley DB tables on start\n\
  --bdb-shared-data	  Start Berkeley DB in multi-process mode\n\
  --bdb-tmpdir=directory  Berkeley DB tempfile name\n\
  --skip-bdb		  Don't use berkeley db (will save memory)\n\
");
#endif
#ifdef HAVE_INNOBASE_DB
  puts("\
  --skip-innobase	  Don't use innobase (will save memory)\n\
");
#endif
  print_defaults("my",load_default_groups);
  puts("");

#include "sslopt-usage.h"

  fix_paths();
  set_ports();
  printf("\
To see what values a running MySQL server is using, type\n\
'mysqladmin variables' instead of 'mysqld --help'.\n\
The default values (after parsing the command line arguments) are:\n\n");

  printf("basedir:     %s\n",mysql_home);
  printf("datadir:     %s\n",mysql_real_data_home);
  printf("tmpdir:      %s\n",mysql_tmpdir);
  printf("language:    %s\n",language);
#ifndef __WIN__
  printf("pid file:    %s\n",pidfile_name);
#endif
  if (opt_logname)
    printf("logfile:     %s\n",opt_logname);
  if (opt_update_logname)
    printf("update log:  %s\n",opt_update_logname);
  if (opt_bin_log)
  {
    printf("binary log:  %s\n",opt_bin_logname ? opt_bin_logname : "");
    printf("binary log index:  %s\n",
	   opt_binlog_index_name ? opt_binlog_index_name : "");
  }
  if (opt_slow_logname)
    printf("update log:  %s\n",opt_slow_logname);
  printf("TCP port:    %d\n",mysql_port);
#if defined(HAVE_SYS_UN_H)
  printf("Unix socket: %s\n",mysql_unix_port);
#endif
  if (my_disable_locking)
    puts("\nsystem locking is not in use");
  if (opt_noacl)
    puts("\nGrant tables are not used. All users have full access rights");
  printf("\nPossible variables for option --set-variable (-O) are:\n");
  for (uint i=0 ; changeable_vars[i].name ; i++)
    printf("%-20s  current value: %lu\n",
	   changeable_vars[i].name,
	   (ulong) *changeable_vars[i].varptr);
}


static void set_options(void)
{
  set_all_changeable_vars( changeable_vars );
#if !defined( my_pthread_setprio ) && !defined( HAVE_PTHREAD_SETSCHEDPARAM )
  opt_specialflag |= SPECIAL_NO_PRIOR;
#endif

  (void) strmov( default_charset, MYSQL_CHARSET);
  (void) strmov( language, LANGUAGE);
  (void) strmov( mysql_real_data_home, get_relative_path(DATADIR));
#ifdef __WIN__
  /* Allow Win32 users to move MySQL anywhere */
  {
    char prg_dev[LIBLEN];
    my_path(prg_dev,my_progname,"mysql/bin");
    strcat(prg_dev,"/../");			// Remove 'bin' to get base dir
    cleanup_dirname(mysql_home,prg_dev);
  }
#else
  const char *tmpenv;
  if ( !(tmpenv = getenv("MY_BASEDIR_VERSION")))
    tmpenv = DEFAULT_MYSQL_HOME;
  (void) strmov( mysql_home, tmpenv );
#endif

#if defined( HAVE_mit_thread ) || defined( __WIN__ ) || defined( HAVE_LINUXTHREADS )
  my_disable_locking = 1;
#endif
  my_bind_addr = htonl( INADDR_ANY );
}

	/* Initiates DEBUG - but no debugging here ! */

static void get_options(int argc,char **argv)
{
  int c,option_index=0;

  myisam_delay_key_write=1;			// Allow use of this
  while ((c=getopt_long(argc,argv,"ab:C:h:#::T::?l::L:O:P:sS::t:u:noVvI?",
			long_options, &option_index)) != EOF)
  {
    switch(c) {
#ifndef DBUG_OFF
    case '#':
      DBUG_PUSH(optarg ? optarg : default_dbug_option);
      opt_endinfo=1;				/* unireg: memory allocation */
      break;
#endif
    case 'a':
      opt_ansi_mode=1;
      thd_startup_options|=OPTION_ANSI_MODE;
      break;
    case 'b':
      strmov(mysql_home,optarg);
      break;
    case 'l':
      opt_log=1;
      opt_logname=optarg;			// Use hostname.log if null
      break;
    case 'h':
      strmov(mysql_real_data_home,optarg);
      break;
    case 'L':
      strmov(language,optarg);
      break;
    case 'n':
      opt_specialflag|= SPECIAL_NEW_FUNC;
      break;
    case 'o':
      protocol_version=PROTOCOL_VERSION-1;
      break;
    case 'O':
      if (set_changeable_var(optarg, changeable_vars))
      {
	use_help();
	exit(1);
      }
      break;
    case 'P':
      mysql_port= (unsigned int) atoi(optarg);
      break;
#if !defined(DBUG_OFF) && defined(SAFEMALLOC)      
    case OPT_SAFEMALLOC_MEM_LIMIT:
      safemalloc_mem_limit = atoi(optarg);
      break;
#endif      
    case OPT_SOCKET:
      mysql_unix_port= optarg;
      break;
    case 'r':
      mysqld_chroot=optarg;
      break;
#ifdef USE_SYMDIR
    case 's':
      my_use_symdir=1;			/* Use internal symbolic links */
      break;
#endif
    case 't':
      mysql_tmpdir=optarg;
      break;
    case 'u':
      mysqld_user=optarg;
      break;
    case 'v':
    case 'V':
      print_version();
      exit(0);
    case 'I':
    case '?':
      usage();
      exit(0);
    case 'T':
      test_flags= optarg ? (uint) atoi(optarg) : 0;
      opt_endinfo=1;
      break;
    case 'S':
      if (!optarg)
	opt_specialflag|= SPECIAL_NO_NEW_FUNC | SPECIAL_SAFE_MODE;
      else if (!strcmp(optarg,"l"))
	my_disable_locking=1;
      else if (!strcmp(optarg,"g"))
	opt_noacl=1;
      else
      {
	fprintf(stderr,"%s: Unrecognized option: %s\n",my_progname,optarg);
	use_help();
	exit(1);
      }
      break;
    case (int) OPT_BIG_TABLES:
      thd_startup_options|=OPTION_BIG_TABLES;
      break;
    case (int) OPT_ISAM_LOG:
      opt_myisam_log=1;
      if (optarg)
	myisam_log_filename=optarg;
      break;
    case (int) OPT_UPDATE_LOG:
      opt_update_log=1;
      opt_update_logname=optarg;		// Use hostname.# if null
      break;
    case (int) OPT_BIN_LOG_INDEX:
      opt_binlog_index_name = optarg;
      break;
    case (int) OPT_BIN_LOG:
      opt_bin_log=1;
      x_free(opt_bin_logname);
      if (optarg && optarg[0])
	opt_bin_logname=my_strdup(optarg,MYF(0));
      break;
      // needs to be handled (as no-op) in non-debugging mode for test suite
    case (int)OPT_DISCONNECT_SLAVE_EVENT_COUNT:
#ifndef DBUG_OFF      
      disconnect_slave_event_count = atoi(optarg);
#endif      
      break;
    case (int)OPT_ABORT_SLAVE_EVENT_COUNT:
#ifndef DBUG_OFF      
      abort_slave_event_count = atoi(optarg);
#endif      
      break;
    case (int) OPT_LOG_SLAVE_UPDATES:
      opt_log_slave_updates = 1;
      break;

    case (int)OPT_REPLICATE_IGNORE_DB:
      {
	i_string *db = new i_string(optarg);
	replicate_ignore_db.push_back(db);
        break;
      }
    case (int)OPT_REPLICATE_DO_DB:
      {
	i_string *db = new i_string(optarg);
	replicate_do_db.push_back(db);
        break;
      }
    case (int)OPT_REPLICATE_REWRITE_DB:
      {
	char* key = optarg,*p, *val;
	p = strstr(optarg, "->");
	if (!p)
	  {
	    fprintf(stderr,
		    "bad syntax in replicate-rewrite-db - missing ->\n");
	    exit(1);
	  }
	val = p--;
	while(isspace(*p) && p > optarg) *p-- = 0;
	if(p == optarg)
	  {
	    fprintf(stderr,
		    "bad syntax in replicate-rewrite-db - empty FROM db\n");
	    exit(1);
	  }
	*val = 0;
	val += 2;
	while(*val && isspace(*val)) *val++;
	if (!*val)
	  {
	    fprintf(stderr,
		    "bad syntax in replicate-rewrite-db - empty TO db\n");
	    exit(1);
	  }

	i_string_pair* db_pair = new i_string_pair(key, val);
	replicate_rewrite_db.push_back(db_pair);
	break;
      }

    case (int)OPT_BINLOG_IGNORE_DB:
      {
	i_string *db = new i_string(optarg);
	binlog_ignore_db.push_back(db);
        break;
      }
    case (int)OPT_BINLOG_DO_DB:
      {
	i_string *db = new i_string(optarg);
	binlog_do_db.push_back(db);
        break;
      }
    case (int)OPT_REPLICATE_DO_TABLE:
      {
	if (!do_table_inited)
	  init_table_rule_hash(&replicate_do_table, &do_table_inited);
	if(add_table_rule(&replicate_do_table, optarg))
	  {
	    fprintf(stderr, "could not add do table rule '%s'\n", optarg);
	    exit(1);
	  }
	table_rules_on = 1;
	break;
      }
    case (int)OPT_REPLICATE_WILD_DO_TABLE:
      {
	if (!wild_do_table_inited)
	  init_table_rule_array(&replicate_wild_do_table,
				&wild_do_table_inited);
	if(add_wild_table_rule(&replicate_wild_do_table, optarg))
	  {
	    fprintf(stderr, "could not add do table rule '%s'\n", optarg);
	    exit(1);
	  }
	table_rules_on = 1;
	break;
      }
    case (int)OPT_REPLICATE_WILD_IGNORE_TABLE:
      {
	if (!wild_ignore_table_inited)
	  init_table_rule_array(&replicate_wild_ignore_table,
				&wild_ignore_table_inited);
	if(add_wild_table_rule(&replicate_wild_ignore_table, optarg))
	  {
	    fprintf(stderr, "could not add do table rule '%s'\n", optarg);
	    exit(1);
	  }
	table_rules_on = 1;
	break;
      }
    case (int)OPT_REPLICATE_IGNORE_TABLE:
      {
	if (!ignore_table_inited)
	  init_table_rule_hash(&replicate_ignore_table, &ignore_table_inited);
	if(add_table_rule(&replicate_ignore_table, optarg))
	  {
	    fprintf(stderr, "could not add ignore table rule '%s'\n", optarg);
	    exit(1);
	  }
	table_rules_on = 1;
	break;
      }
    case (int) OPT_SQL_BIN_UPDATE_SAME:
      opt_sql_bin_update  = 1;
      break;
    case (int) OPT_SLOW_QUERY_LOG:
      opt_slow_log=1;
      opt_slow_logname=optarg;
      break;
    case (int)OPT_SKIP_SLAVE_START:
      opt_skip_slave_start = 1;
      break;
    case (int) OPT_SKIP_NEW:
      opt_specialflag|= SPECIAL_NO_NEW_FUNC;
      default_table_type=DB_TYPE_ISAM;
      myisam_delay_key_write=0;
      myisam_concurrent_insert=0;
      myisam_recover_options= HA_RECOVER_NONE;
      ha_open_options&= ~HA_OPEN_ABORT_IF_CRASHED;
      break;
    case (int) OPT_SAFE:
      opt_specialflag|= SPECIAL_SAFE_MODE;
      myisam_delay_key_write=0;
      myisam_recover_options= HA_RECOVER_NONE;	// To be changed
      ha_open_options&= ~HA_OPEN_ABORT_IF_CRASHED;
      break;
    case (int) OPT_SKIP_CONCURRENT_INSERT:
      myisam_concurrent_insert=0;
      break;
    case (int) OPT_SKIP_PRIOR:
      opt_specialflag|= SPECIAL_NO_PRIOR;
      break;
    case (int) OPT_SKIP_GRANT:
      opt_noacl=1;
      break;
    case (int) OPT_SKIP_LOCK:
      my_disable_locking=1;
      break;
    case (int) OPT_SKIP_HOST_CACHE:
      opt_specialflag|= SPECIAL_NO_HOST_CACHE;
      break;
    case (int) OPT_ENABLE_LOCK:
      my_disable_locking=0;
      break;
    case (int) OPT_USE_LOCKING:
      my_disable_locking=0;
      break;
    case (int) OPT_SKIP_RESOLVE:
      opt_specialflag|=SPECIAL_NO_RESOLVE;
      break;
    case (int) OPT_LONG_FORMAT:
      opt_specialflag|=SPECIAL_LONG_LOG_FORMAT;
      break;
    case (int) OPT_SKIP_NETWORKING:
      opt_disable_networking=1;
      mysql_port=0;
      break;
    case (int) OPT_SKIP_SHOW_DB:
      opt_skip_show_db=1;
      opt_specialflag|=SPECIAL_SKIP_SHOW_DB;
      mysql_port=0;
      break;
    case (int) OPT_MEMLOCK:
      locked_in_memory=1;
      break;
    case (int) OPT_ONE_THREAD:
      test_flags |= TEST_NO_THREADS;
      break;
    case (int) OPT_WANT_CORE:
      test_flags |= TEST_CORE_ON_SIGNAL;
      break;
    case (int) OPT_BIND_ADDRESS:
      if (optarg && isdigit(optarg[0]))
      {
	my_bind_addr = (ulong) inet_addr(optarg);
      }
      else
      {
	struct hostent *ent;
	if (!optarg || !optarg[0])
	  ent=gethostbyname(optarg);
	else
	{
	  char myhostname[255];
	  if (gethostname(myhostname,sizeof(myhostname)) < 0)
	  {
	    sql_perror("Can't start server: cannot get my own hostname!");
	    exit(1);
	  }
	  ent=gethostbyname(myhostname);
	}
	if (!ent)
	{
	  sql_perror("Can't start server: cannot resolve hostname!");
	  exit(1);
	}
	my_bind_addr = (ulong) ((in_addr*)ent->h_addr_list[0])->s_addr;
      }
      break;
    case (int) OPT_PID_FILE:
      strmov(pidfile_name,optarg);
      break;
    case (int) OPT_INIT_FILE:
      opt_init_file=optarg;
      break;
#ifdef __WIN__
    case (int) OPT_STANDALONE:		/* Dummy option for NT */
      break;
    case (int) OPT_CONSOLE:
      opt_console=1;
      break;
#endif
    case (int) OPT_FLUSH:
      nisam_flush=myisam_flush=1;
      flush_time=0;			// No auto flush
      break;
    case OPT_LOW_PRIORITY_UPDATES:
      thd_startup_options|=OPTION_LOW_PRIORITY_UPDATES;
      low_priority_updates=1;
      break;
    case OPT_BOOTSTRAP:
      opt_noacl=opt_bootstrap=1;
      break;
    case OPT_TABLE_TYPE:
    {
      int type;
      if ((type=find_type(optarg, &ha_table_typelib, 2)) <= 0)
      {
	fprintf(stderr,"Unknown table type: %s\n",optarg);
	exit(1);
      }
      default_table_type= (enum db_type) type;
      break;
    }
    case OPT_SERVER_ID:
      server_id = atoi(optarg);
      server_id_supplied = 1;
      break;
    case OPT_DELAY_KEY_WRITE:
      ha_open_options|=HA_OPEN_DELAY_KEY_WRITE;
      myisam_delay_key_write=1;
      break;
    case OPT_SKIP_DELAY_KEY_WRITE:
      myisam_delay_key_write=0;
      break;
    case 'C':
      strmov(default_charset,optarg);
      break;
    case OPT_CHARSETS_DIR:
      strmov(mysql_charsets_dir, optarg);
      charsets_dir = mysql_charsets_dir;
      break;
#include "sslopt-case.h"
#ifdef HAVE_BERKELEY_DB
    case OPT_BDB_LOG:
      berkeley_logdir=optarg;
      break;
    case OPT_BDB_HOME:
      berkeley_home=optarg;
      break;
    case OPT_BDB_NOSYNC:
      berkeley_env_flags|=DB_TXN_NOSYNC;
      break;
    case OPT_BDB_NO_RECOVER:
      berkeley_init_flags&= ~(DB_RECOVER);
      break;
    case OPT_BDB_TMP:
      berkeley_tmpdir=optarg;
      break;
    case OPT_BDB_LOCK:
    {
      int type;
      if ((type=find_type(optarg, &berkeley_lock_typelib, 2)) > 0)
	berkeley_lock_type=berkeley_lock_types[type-1];
      else
      {
	if (test_if_int(optarg,(uint) strlen(optarg)))
	  berkeley_lock_scan_time=atoi(optarg);
	else
	{
	  fprintf(stderr,"Unknown lock type: %s\n",optarg);
	  exit(1);
	}
      }
      break;
    }
    case OPT_BDB_SHARED:
      berkeley_init_flags&= ~(DB_PRIVATE);
      berkeley_shared_data=1;
      break;
    case OPT_BDB_SKIP:
      berkeley_skip=1;
      have_berkeley_db=SHOW_OPTION_DISABLED;
      break;
#endif
#ifdef HAVE_GEMINI_DB
    case OPT_GEMINI_SKIP:
      gemini_skip=1;
      have_gemini_db=SHOW_OPTION_DISABLED;  
      break;
#endif
#ifdef HAVE_INNOBASE_DB
    case OPT_INNOBASE_SKIP:
      innobase_skip=1;
      have_innobase_db=SHOW_HAVE_DISABLED;
      break;
    case OPT_INNOBASE_DATA_HOME_DIR:
      innobase_data_home_dir=optarg;
      break;
    case OPT_INNOBASE_DATA_FILE_PATH:
      innobase_data_file_path=optarg;
      break;
    case OPT_INNOBASE_LOG_GROUP_HOME_DIR:
      innobase_log_group_home_dir=optarg;
      break;
    case OPT_INNOBASE_LOG_ARCH_DIR:
      innobase_log_arch_dir=optarg;
      break;
    case OPT_INNOBASE_LOG_ARCHIVE:
      innobase_log_archive= optarg ? test(atoi(optarg)) : 1;
      break;
    case OPT_INNOBASE_FLUSH_LOG_AT_TRX_COMMIT:
      innobase_flush_log_at_trx_commit= optarg ? test(atoi(optarg)) : 1;
      break;
#endif
    case OPT_MYISAM_RECOVER:
    {
      if (!optarg || !optarg[0])
      {
	myisam_recover_options=    HA_RECOVER_DEFAULT;
	myisam_recover_options_str= myisam_recover_typelib.type_names[0];
      }
      else
      {
	myisam_recover_options_str=optarg;
	if ((myisam_recover_options=
		find_bit_type(optarg, &myisam_recover_typelib)) == ~(ulong) 0)
	{
	  fprintf(stderr, "Unknown option to myisam-recover: %s\n",optarg);
	  exit(1);
	}
      }
      ha_open_options|=HA_OPEN_ABORT_IF_CRASHED;
      break;
    }
    case OPT_MASTER_HOST:
      master_host=optarg;
      break;
    case OPT_MASTER_USER:
      master_user=optarg;
      break;
    case OPT_MASTER_PASSWORD:
      master_password=optarg;
      break;
    case OPT_MASTER_INFO_FILE:
      master_info_file=optarg;
      break;
    case OPT_MASTER_PORT:
      master_port= atoi(optarg);
      break;
    case OPT_MASTER_CONNECT_RETRY:
      master_connect_retry= atoi(optarg);
      break;
    case (int) OPT_SAFE_SHOW_DB:
      opt_safe_show_db=1;
      break;

    default:
      fprintf(stderr,"%s: Unrecognized option: %c\n",my_progname,c);
      use_help();
      exit(1);
    }
  }
  // Skipp empty arguments (from shell)
  while (argc != optind && !argv[optind][0])
    optind++;
  if (argc != optind)
  {
    fprintf(stderr,"%s: Too many parameters\n",my_progname);
    use_help();
    exit(1);
  }
  fix_paths();
  default_table_type_name=ha_table_typelib.type_names[default_table_type-1];
}


#ifdef __WIN__

#ifndef KEY_SERVICE_PARAMETERS
#define KEY_SERVICE_PARAMETERS	"SYSTEM\\CurrentControlSet\\Services\\MySql\\Parameters"
#endif

#define COPY_KEY_VALUE(value) if (copy_key_value(hParametersKey,&(value),lpszValue)) return 1
#define CHECK_KEY_TYPE(type,name) if ( type != dwKeyValueType ) { key_type_error(hParametersKey,name); return 1; }
#define SET_CHANGEABLE_VARVAL(varname) if (set_varval(hParametersKey,varname,szKeyValueName,dwKeyValueType,lpdwValue)) return 1;

static void key_type_error(HKEY hParametersKey,const char *szKeyValueName)
{
 TCHAR szErrorMsg[512];
 RegCloseKey( hParametersKey );
 strxmov(szErrorMsg,TEXT("Value \""),
	 szKeyValueName,
	 TEXT("\" of registry key \"" KEY_SERVICE_PARAMETERS "\" has wrong type\n"),NullS);
 fprintf(stderr, szErrorMsg); /* not unicode compatible */
}

static bool copy_key_value(HKEY hParametersKey, char **var, const char *value)
{
  if (!(*var=my_strdup(value,MYF(MY_WME))))
  {
    RegCloseKey(hParametersKey);
    fprintf(stderr, "Couldn't allocate memory for registry key value\n");
    return 1;
  }
  return 0;
}

static bool set_varval(HKEY hParametersKey,const char *var,
		       const char *szKeyValueName, DWORD dwKeyValueType,
		       LPDWORD lpdwValue)
{
  CHECK_KEY_TYPE(dwKeyValueType, szKeyValueName );
  if (set_changeable_varval(var, *lpdwValue, changeable_vars))
  {
    TCHAR szErrorMsg [ 512 ];
    RegCloseKey( hParametersKey );
    strxmov(szErrorMsg,
	    TEXT("Value \""),
	    szKeyValueName,
	    TEXT("\" of registry key \"" KEY_SERVICE_PARAMETERS  "\" is invalid\n"),NullS);
    fprintf( stderr, szErrorMsg ); /* not unicode compatible */
    return 1;
  }
  return 0;
}


static int get_service_parameters()
{
  DWORD dwLastError;
  HKEY hParametersKey;
  DWORD dwIndex;
  TCHAR szKeyValueName [ 256 ];
  DWORD dwKeyValueName;
  DWORD dwKeyValueType;
  BYTE bKeyValueBuffer [ 512 ];
  DWORD dwKeyValueBuffer;
  LPDWORD lpdwValue = (LPDWORD) &bKeyValueBuffer[0];
  LPCTSTR lpszValue = (LPCTSTR) &bKeyValueBuffer[0];

  /* open parameters of service */
  dwLastError = (DWORD) RegOpenKeyEx( HKEY_LOCAL_MACHINE,
				      TEXT(KEY_SERVICE_PARAMETERS), 0,
				      KEY_READ, &hParametersKey );
  if ( dwLastError == ERROR_FILE_NOT_FOUND ) /* no parameters available */
    return 0;
  if ( dwLastError != ERROR_SUCCESS )
  {
    fprintf(stderr,"Can't open registry key \"" KEY_SERVICE_PARAMETERS "\" for reading\n" );
    return 1;
  }

  /* enumerate all values of key */
  dwIndex = 0;
  dwKeyValueName = sizeof( szKeyValueName ) / sizeof( TCHAR );
  dwKeyValueBuffer = sizeof( bKeyValueBuffer );
  while ( (dwLastError = (DWORD) RegEnumValue(hParametersKey, dwIndex,
					      szKeyValueName, &dwKeyValueName,
					      NULL, &dwKeyValueType,
					      &bKeyValueBuffer[0],
					      &dwKeyValueBuffer))
	  != ERROR_NO_MORE_ITEMS )
  {
    /* check if error occured */
    if ( dwLastError != ERROR_SUCCESS )
    {
      RegCloseKey( hParametersKey );
      fprintf( stderr, "Can't enumerate values of registry key \"" KEY_SERVICE_PARAMETERS "\"\n" );
      return 1;
    }
    if ( lstrcmp(szKeyValueName, TEXT("BaseDir")) == 0 )
    {
      CHECK_KEY_TYPE( REG_SZ, szKeyValueName);
      strmov( mysql_home, lpszValue ); /* not unicode compatible */
    }
    else if ( lstrcmp(szKeyValueName, TEXT("BindAddress")) == 0 )
    {
      CHECK_KEY_TYPE( REG_SZ, szKeyValueName);

      my_bind_addr = (ulong) inet_addr( lpszValue );
      if ( my_bind_addr == (ulong) INADDR_NONE )
      {
	struct hostent* ent;

	if ( !(*lpszValue) )
	{
	  char szHostName [ 256 ];
	  if ( gethostname(szHostName, sizeof(szHostName)) == SOCKET_ERROR )
	  {
	    RegCloseKey( hParametersKey );
	    fprintf( stderr, "Can't get my own hostname\n" );
	    return 1;
	  }
	  ent = gethostbyname( szHostName );
	}
	else ent = gethostbyname( lpszValue );
	if ( !ent )
	{
	  RegCloseKey( hParametersKey );
	  fprintf( stderr, "Can't resolve hostname!\n" );
	  return 1;
	}
	my_bind_addr = (ulong) ((in_addr*)ent->h_addr_list[0])->s_addr;
      }
    }
    else if ( lstrcmp(szKeyValueName, TEXT("BigTables")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName);
      if ( *lpdwValue )
	thd_startup_options |= OPTION_BIG_TABLES;
      else
	thd_startup_options &= ~((ulong)OPTION_BIG_TABLES);
    }
    else if ( lstrcmp(szKeyValueName, TEXT("DataDir")) == 0 )
    {
      CHECK_KEY_TYPE( REG_SZ, szKeyValueName );
      strmov( mysql_real_data_home, lpszValue ); /* not unicode compatible */
    }
    else if ( lstrcmp(szKeyValueName, TEXT("Locking")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName );
      my_disable_locking = !(*lpdwValue);
    }
    else if ( lstrcmp(szKeyValueName, TEXT("LogFile")) == 0 )
    {
      CHECK_KEY_TYPE( REG_SZ, szKeyValueName );
      opt_log = 1;
      COPY_KEY_VALUE( opt_logname );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("UpdateLogFile")) == 0 )
    {
      CHECK_KEY_TYPE( REG_SZ, szKeyValueName );
      opt_update_log = 1;
      COPY_KEY_VALUE( opt_update_logname );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("BinaryLogFile")) == 0 )
    {
      CHECK_KEY_TYPE( REG_SZ, szKeyValueName );
      opt_bin_log = 1;
      COPY_KEY_VALUE( opt_bin_logname );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("BinaryLogIndexFile")) == 0 )
    {
      CHECK_KEY_TYPE( REG_SZ, szKeyValueName );
      opt_bin_log = 1;
      COPY_KEY_VALUE( opt_binlog_index_name );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("ISAMLogFile")) == 0 )
    {
      CHECK_KEY_TYPE( REG_SZ, szKeyValueName );
      COPY_KEY_VALUE( myisam_log_filename );
      opt_myisam_log=1;
    }
    else if ( lstrcmp(szKeyValueName, TEXT("LongLogFormat")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName );
      if ( *lpdwValue )
	opt_specialflag |= SPECIAL_LONG_LOG_FORMAT;
      else
	opt_specialflag &= ~((ulong)SPECIAL_LONG_LOG_FORMAT);
    }
    else if ( lstrcmp(szKeyValueName, TEXT("LowPriorityUpdates")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName );
      if ( *lpdwValue )
      {
	thd_startup_options |= OPTION_LOW_PRIORITY_UPDATES;
	low_priority_updates = 1;
      }
      else
      {
	thd_startup_options &= ~((ulong)OPTION_LOW_PRIORITY_UPDATES);
	low_priority_updates = 0;
      }
    }
    else if ( lstrcmp(szKeyValueName, TEXT("Port")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName );
      mysql_port = (unsigned int) *lpdwValue;
    }
    else if ( lstrcmp(szKeyValueName, TEXT("OldProtocol")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName );
      protocol_version = *lpdwValue ? PROTOCOL_VERSION - 1 : PROTOCOL_VERSION;
    }
    else if ( lstrcmp(szKeyValueName, TEXT("HostnameResolving")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName );
      if ( !*lpdwValue )
	opt_specialflag |= SPECIAL_NO_RESOLVE;
      else
	opt_specialflag &= ~((ulong)SPECIAL_NO_RESOLVE);
    }
    else if ( lstrcmp(szKeyValueName, TEXT("Networking")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName );
      opt_disable_networking = !(*lpdwValue);
    }
    else if ( lstrcmp(szKeyValueName, TEXT("ShowDatabase")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName );
      opt_skip_show_db = !(*lpdwValue);
    }
    else if ( lstrcmp(szKeyValueName, TEXT("HostnameCaching")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName );
      if ( !*lpdwValue )
	opt_specialflag |= SPECIAL_NO_HOST_CACHE;
      else
	opt_specialflag &= ~((ulong)SPECIAL_NO_HOST_CACHE);
    }
    else if ( lstrcmp(szKeyValueName, TEXT("ThreadPriority")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName );
      if ( !(*lpdwValue) )
	opt_specialflag |= SPECIAL_NO_PRIOR;
      else
	opt_specialflag &= ~((ulong)SPECIAL_NO_PRIOR);
    }
    else if ( lstrcmp(szKeyValueName, TEXT("NamedPipe")) == 0 )
    {
      CHECK_KEY_TYPE( REG_SZ, szKeyValueName );
      COPY_KEY_VALUE( mysql_unix_port );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("TempDir")) == 0 )
    {
      CHECK_KEY_TYPE( REG_SZ, szKeyValueName );
      COPY_KEY_VALUE( mysql_tmpdir );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("FlushTables")) == 0 )
    {
      CHECK_KEY_TYPE( REG_DWORD, szKeyValueName );
      nisam_flush = myisam_flush= *lpdwValue ? 1 : 0;
    }
    else if ( lstrcmp(szKeyValueName, TEXT("BackLog")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "back_log" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("ConnectTimeout")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "connect_timeout" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("JoinBufferSize")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "join_buffer" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("KeyBufferSize")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "key_buffer" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("LongQueryTime")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "long_query_time" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("MaxAllowedPacket")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "max_allowed_packet" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("MaxConnections")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "max_connections" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("MaxConnectErrors")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "max_connect_errors" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("MaxInsertDelayedThreads")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "max_delayed_threads" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("MaxJoinSize")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "max_join_size" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("MaxSortLength")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "max_sort_length" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("NetBufferLength")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "net_buffer_length" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("RecordBufferSize")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "record_buffer" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("SortBufferSize")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "sort_buffer" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("TableCacheSize")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "table_cache" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("TmpTableSize")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "tmp_table_size" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("ThreadStackSize")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "thread_stack" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("WaitTimeout")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "wait_timeout" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("DelayedInsertTimeout"))
	      == 0 )
    {
      SET_CHANGEABLE_VARVAL( "delayed_insert_timeout" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("DelayedInsertLimit")) ==
	      0 )
    {
      SET_CHANGEABLE_VARVAL( "delayed_insert_limit" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("DelayedQueueSize")) == 0
	      )
    {
      SET_CHANGEABLE_VARVAL( "delayed_queue_size" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("FlushTime")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "flush_time" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("InteractiveTimeout")) ==
	      0 )
    {
      SET_CHANGEABLE_VARVAL( "interactive_timeout" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("LowerCaseTableNames"))
	      == 0 )
    {
      SET_CHANGEABLE_VARVAL( "lower_case_table_names" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("MaxHeapTableSize")) == 0
	      )
    {
      SET_CHANGEABLE_VARVAL( "max_heap_table_size" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("MaxTmpTables")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "max_tmp_tables" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("MaxWriteLockCount")) ==
	      0 )
    {
      SET_CHANGEABLE_VARVAL( "max_write_lock_count" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("NetRetryCount")) == 0 )
    {
      SET_CHANGEABLE_VARVAL( "net_retry_count" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("QueryBufferSize")) == 0
	      )
    {
      SET_CHANGEABLE_VARVAL( "query_buffer_size" );
    }
    else if ( lstrcmp(szKeyValueName, TEXT("ThreadConcurrency")) ==
	      0 )
    {
      SET_CHANGEABLE_VARVAL( "thread_concurrency" );
    }
    else
    {
      TCHAR szErrorMsg [ 512 ];
      RegCloseKey( hParametersKey );
      lstrcpy( szErrorMsg, TEXT("Value \"") );
      lstrcat( szErrorMsg, szKeyValueName );
      lstrcat( szErrorMsg, TEXT("\" of registry key \"" KEY_SERVICE_PARAMETERS "\" is not defined by MySQL\n") );
      fprintf( stderr, szErrorMsg ); /* not unicode compatible */
      return 1;
    }

    dwIndex++;
    dwKeyValueName = sizeof( szKeyValueName ) / sizeof( TCHAR );
    dwKeyValueBuffer = sizeof( bKeyValueBuffer );
  }
  RegCloseKey( hParametersKey );

  /* paths are fixed by method get_options() */
  return 0;
}
#endif


static char *get_relative_path(const char *path)
{
  if (test_if_hard_path(path) &&
      is_prefix(path,DEFAULT_MYSQL_HOME) &&
      strcmp(DEFAULT_MYSQL_HOME,FN_ROOTDIR))
  {
    path+=(uint) strlen(DEFAULT_MYSQL_HOME);
    while (*path == FN_LIBCHAR)
      path++;
  }
  return (char*) path;
}


static void fix_paths(void)
{
  (void) fn_format(mysql_home,mysql_home,"","",16); // Remove symlinks
  convert_dirname(mysql_home);
  convert_dirname(mysql_real_data_home);
  convert_dirname(language);
  (void) my_load_path(mysql_home,mysql_home,""); // Resolve current dir
  (void) my_load_path(mysql_real_data_home,mysql_real_data_home,mysql_home);
  (void) my_load_path(pidfile_name,pidfile_name,mysql_real_data_home);

  char buff[FN_REFLEN],*sharedir=get_relative_path(SHAREDIR);
  if (test_if_hard_path(sharedir))
    strmov(buff,sharedir);			/* purecov: tested */
  else
    strxmov(buff,mysql_home,sharedir,NullS);
  convert_dirname(buff);
  (void) my_load_path(language,language,buff);

  /* If --character-sets-dir isn't given, use shared library dir */
  if (charsets_dir != mysql_charsets_dir)
  {
    strmov(strmov(mysql_charsets_dir,buff),CHARSET_DIR);
    charsets_dir=mysql_charsets_dir;
  }

  /* Add '/' to TMPDIR if needed */
  char *tmp= (char*) my_malloc(FN_REFLEN,MYF(MY_FAE));
  if (tmp)
  {
    strmov(tmp,mysql_tmpdir);
    mysql_tmpdir=tmp;
    convert_dirname(mysql_tmpdir);
    mysql_tmpdir=(char*) my_realloc(mysql_tmpdir,(uint) strlen(mysql_tmpdir)+1,
				    MYF(MY_HOLD_ON_ERROR));
  }
}


#ifdef SET_RLIMIT_NOFILE
static uint set_maximum_open_files(uint max_file_limit)
{
  struct rlimit rlimit;
  ulong old_cur;

  if (!getrlimit(RLIMIT_NOFILE,&rlimit))
  {
    old_cur=rlimit.rlim_cur;
    if (rlimit.rlim_cur >= max_file_limit)	// Nothing to do
      return rlimit.rlim_cur;			/* purecov: inspected */
    rlimit.rlim_cur=rlimit.rlim_max=max_file_limit;
    if (setrlimit(RLIMIT_NOFILE,&rlimit))
    {
      sql_print_error("Warning: setrlimit couldn't increase number of open files to more than %ld",
	      old_cur);		/* purecov: inspected */
      max_file_limit=old_cur;
    }
    else
    {
      (void) getrlimit(RLIMIT_NOFILE,&rlimit);
      if ((uint) rlimit.rlim_cur != max_file_limit)
	sql_print_error("Warning: setrlimit returned ok, but didn't change limits. Max open files is %ld",
			  (ulong) rlimit.rlim_cur); /* purecov: inspected */
      max_file_limit=rlimit.rlim_cur;
    }
  }
  return max_file_limit;
}
#endif


	/*
	  Return a bitfield from a string of substrings separated by ','
	  returns ~(ulong) 0 on error.
	*/

static ulong find_bit_type(const char *x, TYPELIB *bit_lib)
{
  bool found_end;
  int  found_count;
  const char *end,*i,*j;
  const char **array, *pos;
  ulong found,found_int,bit;
  DBUG_ENTER("find_bit_type");
  DBUG_PRINT("enter",("x: '%s'",x));

  found=0;
  found_end= 0;
  pos=(my_string) x;
  do
  {
    if (!*(end=strcend(pos,',')))		/* Let end point at fieldend */
    {
      while (end > pos && end[-1] == ' ')
	end--;					/* Skipp end-space */
      found_end=1;
    }
    found_int=0; found_count=0;
    for (array=bit_lib->type_names, bit=1 ; (i= *array++) ; bit<<=1)
    {
      j=pos;
      while (j != end)
      {
	if (toupper(*i++) != toupper(*j++))
	  goto skipp;
      }
      found_int=bit;
      if (! *i)
      {
	found_count=1;
	break;
      }
      else if (j != pos)			// Half field found
      {
	found_count++;				// Could be one of two values
      }
skipp: ;
    }
    if (found_count != 1)
      DBUG_RETURN(~(ulong) 0);				// No unique value
    found|=found_int;
    pos=end+1;
  } while (! found_end);

  DBUG_PRINT("exit",("bit-field: %ld",(ulong) found));
  DBUG_RETURN(found);
} /* find_bit_type */


/*****************************************************************************
** Instantiate templates
*****************************************************************************/

#ifdef __GNUC__
/* Used templates */
template class I_List<THD>;
template class I_List_iterator<THD>;
template class I_List<i_string>;
template class I_List<i_string_pair>;
#endif

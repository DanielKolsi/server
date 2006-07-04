/* Copyright (C) 2004-2006 MySQL AB

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
#include "events.h"
#include "event_data_objects.h"
#include "event_scheduler.h"
#include "event_db_repository.h"
#include "sp_head.h"
#include "event_queue.h"


#ifdef __GNUC__
#if __GNUC__ >= 2
#define SCHED_FUNC __FUNCTION__
#endif
#else
#define SCHED_FUNC "<unknown>"
#endif

#define LOCK_SCHEDULER_DATA()   lock_data(SCHED_FUNC, __LINE__)
#define UNLOCK_SCHEDULER_DATA() unlock_data(SCHED_FUNC, __LINE__)


Event_scheduler*
Event_scheduler::singleton= NULL;


#ifndef DBUG_OFF
static
LEX_STRING states_names[] =
{
  {(char*) STRING_WITH_LEN("UNINITIALIZED")},
  {(char*) STRING_WITH_LEN("INITIALIZED")},
  {(char*) STRING_WITH_LEN("COMMENCING")},
  {(char*) STRING_WITH_LEN("CANTSTART")},
  {(char*) STRING_WITH_LEN("RUNNING")},
  {(char*) STRING_WITH_LEN("SUSPENDED")},
  {(char*) STRING_WITH_LEN("IN_SHUTDOWN")}
};
#endif

const char * const
Event_scheduler::cond_vars_names[Event_scheduler::COND_LAST] =
{
  "new work",
  "started or stopped",
  "suspend or resume"
};


/*
Event_scheduler*
Event_scheduler::singleton= NULL;
*/



class Worker_thread_param
{
public:
  Event_timed *et;
  pthread_mutex_t LOCK_started;
  pthread_cond_t COND_started;
  bool started;

  Worker_thread_param(Event_timed *etn):et(etn), started(FALSE)
  {
    pthread_mutex_init(&LOCK_started, MY_MUTEX_INIT_FAST);
    pthread_cond_init(&COND_started, NULL);  
  }

  ~Worker_thread_param()
  {
    pthread_mutex_destroy(&LOCK_started);
    pthread_cond_destroy(&COND_started);
  }
};


/*
  Prints the stack of infos, warnings, errors from thd to
  the console so it can be fetched by the logs-into-tables and
  checked later.

  SYNOPSIS
    evex_print_warnings
      thd    - thread used during the execution of the event
      et     - the event itself
*/

static void
evex_print_warnings(THD *thd, Event_timed *et)
{
  MYSQL_ERROR *err;
  DBUG_ENTER("evex_print_warnings");
  if (!thd->warn_list.elements)
    DBUG_VOID_RETURN;

  char msg_buf[10 * STRING_BUFFER_USUAL_SIZE];
  char prefix_buf[5 * STRING_BUFFER_USUAL_SIZE];
  String prefix(prefix_buf, sizeof(prefix_buf), system_charset_info);
  prefix.length(0);
  prefix.append("SCHEDULER: [");

  append_identifier(thd, &prefix, et->definer_user.str, et->definer_user.length);
  prefix.append('@');
  append_identifier(thd, &prefix, et->definer_host.str, et->definer_host.length);
  prefix.append("][", 2);
  append_identifier(thd,&prefix, et->dbname.str, et->dbname.length);
  prefix.append('.');
  append_identifier(thd,&prefix, et->name.str, et->name.length);
  prefix.append("] ", 2);

  List_iterator_fast<MYSQL_ERROR> it(thd->warn_list);
  while ((err= it++))
  {
    String err_msg(msg_buf, sizeof(msg_buf), system_charset_info);
    /* set it to 0 or we start adding at the end. That's the trick ;) */
    err_msg.length(0);
    err_msg.append(prefix);
    err_msg.append(err->msg, strlen(err->msg), system_charset_info);
    err_msg.append("]");
    DBUG_ASSERT(err->level < 3);
    (sql_print_message_handlers[err->level])("%*s", err_msg.length(),
                                              err_msg.c_ptr());
  }
  DBUG_VOID_RETURN;
}


/*
  Inits an scheduler thread handler, both the main and a worker

  SYNOPSIS
    init_event_thread()
      thd - the THD of the thread. Has to be allocated by the caller.

  NOTES
    1. The host of the thead is my_localhost
    2. thd->net is initted with NULL - no communication.

  RETURN VALUE
    0  OK
   -1  Error
*/

static int
init_event_thread(THD** t, enum enum_thread_type thread_type)
{
  THD *thd= *t;
  thd->thread_stack= (char*)t;                  // remember where our stack is
  DBUG_ENTER("init_event_thread");
  thd->client_capabilities= 0;
  thd->security_ctx->master_access= 0;
  thd->security_ctx->db_access= 0;
  thd->security_ctx->host_or_ip= (char*)my_localhost;
  my_net_init(&thd->net, 0);
  thd->net.read_timeout= slave_net_timeout;
  thd->slave_thread= 0;
  thd->options|= OPTION_AUTO_IS_NULL;
  thd->client_capabilities|= CLIENT_MULTI_RESULTS;
  thd->real_id=pthread_self();
  VOID(pthread_mutex_lock(&LOCK_thread_count));
  thd->thread_id= thread_id++;
  threads.append(thd);
  thread_count++;
  thread_running++;
  VOID(pthread_mutex_unlock(&LOCK_thread_count));

  if (init_thr_lock() || thd->store_globals())
  {
    thd->cleanup();
    DBUG_RETURN(-1);
  }

#if !defined(__WIN__) && !defined(OS2) && !defined(__NETWARE__)
  sigset_t set;
  VOID(sigemptyset(&set));			// Get mask in use
  VOID(pthread_sigmask(SIG_UNBLOCK,&set,&thd->block_signals));
#endif

  /*
    Guarantees that we will see the thread in SHOW PROCESSLIST though its
    vio is NULL.
  */
  thd->system_thread= thread_type;

  thd->proc_info= "Initialized";
  thd->version= refresh_version;
  thd->set_time();

  DBUG_RETURN(0);
}


/*
  Inits the main scheduler thread and then calls Event_scheduler::run()
  of arg. 

  SYNOPSIS
    event_scheduler_thread()
      arg  void* ptr to Event_scheduler

  NOTES
    1. The host of the thead is my_localhost
    2. thd->net is initted with NULL - no communication.
    3. The reason to have a proxy function is that it's not possible to
       use a method as function to be executed in a spawned thread:
       - our pthread_hander_t macro uses extern "C"
       - separating thread setup from the real execution loop is also to be
         considered good.

  RETURN VALUE
    0  OK
*/

pthread_handler_t
event_scheduler_thread(void *arg)
{
  /* needs to be first for thread_stack */
  THD *thd= NULL;                               
  Event_scheduler *scheduler= (Event_scheduler *) arg;

  DBUG_ENTER("event_scheduler_thread");

  my_thread_init();
  pthread_detach_this_thread();

  /* note that constructor of THD uses DBUG_ ! */
  if (!(thd= new THD) || init_event_thread(&thd, SYSTEM_THREAD_EVENT_SCHEDULER))
  {
    sql_print_error("SCHEDULER: Cannot init manager event thread.");
    scheduler->report_error_during_start();
  }
  else
  {
    thd->security_ctx->set_user((char*)"event_scheduler");

    sql_print_information("SCHEDULER: Manager thread booting");
    if (Event_scheduler::get_instance()->event_queue->check_system_tables(thd))
      scheduler->report_error_during_start();
    else
      scheduler->run(thd);

    /*
      NOTE: Don't touch `scheduler` after this point because we have notified
            the
            thread which shuts us down that we have finished cleaning. In this
            very moment a new scheduler thread could be started and a crash is
            not welcome.
    */
  }

  /*
    If we cannot create THD then don't decrease because we haven't touched
    thread_count and thread_running in init_event_thread() which was never
    called. In init_event_thread() thread_count and thread_running are
    always increased even in the case the method returns an error.
  */
  if (thd)
  {
    thd->proc_info= "Clearing";
    DBUG_ASSERT(thd->net.buff != 0);
    net_end(&thd->net);
    pthread_mutex_lock(&LOCK_thread_count);
    thread_count--;
    thread_running--;
    delete thd;
    pthread_mutex_unlock(&LOCK_thread_count);
  }
  my_thread_end();
  DBUG_RETURN(0);                               // Can't return anything here
}


/*
  Function that executes an event in a child thread. Setups the 
  environment for the event execution and cleans after that.

  SYNOPSIS
    event_worker_thread()
      arg  The Event_timed object to be processed

  RETURN VALUE
    0  OK
*/

pthread_handler_t
event_worker_thread(void *arg)
{
  THD *thd; /* needs to be first for thread_stack */
  Worker_thread_param *param= (Worker_thread_param *) arg;
  Event_timed *event= param->et;
  int ret;
  bool startup_error= FALSE;
  Security_context *save_ctx;
  /* this one is local and not needed after exec */
  Security_context security_ctx;

  DBUG_ENTER("event_worker_thread");
  DBUG_PRINT("enter", ("event=[%s.%s]", event->dbname.str, event->name.str));

  my_thread_init();
  pthread_detach_this_thread();

  if (!(thd= new THD) || init_event_thread(&thd, SYSTEM_THREAD_EVENT_WORKER))
  {
    sql_print_error("SCHEDULER: Startup failure.");
    startup_error= TRUE;
    event->spawn_thread_finish(thd);
  }
  else
    event->set_thread_id(thd->thread_id);

  DBUG_PRINT("info", ("master_access=%d db_access=%d",
             thd->security_ctx->master_access, thd->security_ctx->db_access));
  /*
    If we don't change it before we send the signal back, then an intermittent
    DROP EVENT will take LOCK_scheduler_data and try to kill this thread, because
    event->thread_id is already real. However, because thd->security_ctx->user
    is not initialized then a crash occurs in kill_one_thread(). Thus, we have
    to change the context before sending the signal. We are under
    LOCK_scheduler_data being held by Event_scheduler::run() -> ::execute_top().
  */
  thd->change_security_context(event->definer_user, event->definer_host,
                               event->dbname, &security_ctx, &save_ctx);
  DBUG_PRINT("info", ("master_access=%d db_access=%d",
             thd->security_ctx->master_access, thd->security_ctx->db_access));

  /* Signal the scheduler thread that we have started successfully */
  pthread_mutex_lock(&param->LOCK_started);
  param->started= TRUE;
  pthread_cond_signal(&param->COND_started);
  pthread_mutex_unlock(&param->LOCK_started);

  if (!startup_error)
  {
    thd->init_for_queries();
    thd->enable_slow_log= TRUE;

    event->set_thread_id(thd->thread_id);
    sql_print_information("SCHEDULER: [%s.%s of %s] executing in thread %lu",
                          event->dbname.str, event->name.str,
                          event->definer.str, thd->thread_id);

    ret= event->execute(thd, thd->mem_root);
    evex_print_warnings(thd, event);
    sql_print_information("SCHEDULER: [%s.%s of %s] executed. RetCode=%d",
                          event->dbname.str, event->name.str,
                          event->definer.str, ret);
    if (ret == EVEX_COMPILE_ERROR)
      sql_print_information("SCHEDULER: COMPILE ERROR for event %s.%s of %s",
                            event->dbname.str, event->name.str,
                            event->definer.str);
    else if (ret == EVEX_MICROSECOND_UNSUP)
      sql_print_information("SCHEDULER: MICROSECOND is not supported");
    
    DBUG_PRINT("info", ("master_access=%d db_access=%d",
               thd->security_ctx->master_access, thd->security_ctx->db_access));

    /* If true is returned, we are expected to free it */
    if (event->spawn_thread_finish(thd))
    {
      DBUG_PRINT("info", ("Freeing object pointer"));
      delete event;
    }
  }

  if (thd)
  {
    thd->proc_info= "Clearing";
    DBUG_ASSERT(thd->net.buff != 0);
    /*
      Free it here because net.vio is NULL for us => THD::~THD will check it
      and won't call net_end(&net); See also replication code.
    */
    net_end(&thd->net);
    DBUG_PRINT("info", ("Worker thread %lu exiting", thd->thread_id));
    VOID(pthread_mutex_lock(&LOCK_thread_count));
    thread_count--;
    thread_running--;
    delete thd;
    VOID(pthread_mutex_unlock(&LOCK_thread_count));
  }

  my_thread_end();
  DBUG_RETURN(0);                               // Can't return anything here
}




/*
  Constructor of class Event_scheduler.

  SYNOPSIS
    Event_scheduler::Event_scheduler()
*/

Event_scheduler::Event_scheduler()
{
  thread_id= 0;
  mutex_last_unlocked_at_line= mutex_last_locked_at_line= 0;
  mutex_last_unlocked_in_func= mutex_last_locked_in_func= "";
  cond_waiting_on= COND_NONE;
  mutex_scheduler_data_locked= FALSE;
  state= UNINITIALIZED;
  start_scheduler_suspended= FALSE;
  LOCK_scheduler_data= &LOCK_data;
}



/*
  Returns the singleton instance of the class.

  SYNOPSIS
    Event_scheduler::create_instance()

  RETURN VALUE
    address
*/

void
Event_scheduler::create_instance(Event_queue *queue)
{
  singleton= new Event_scheduler();
  singleton->event_queue= queue;
}

/*
  Returns the singleton instance of the class.

  SYNOPSIS
    Event_scheduler::get_instance()

  RETURN VALUE
    address
*/

Event_scheduler*
Event_scheduler::get_instance()
{
  DBUG_ENTER("Event_scheduler::get_instance");
  DBUG_RETURN(singleton);
}


/*
  The implementation of full-fledged initialization.

  SYNOPSIS
    Event_scheduler::init()

  RETURN VALUE
    FALSE  OK
    TRUE   Error
*/

bool
Event_scheduler::init(Event_db_repository *db_repo)
{
  int i= 0;
  bool ret= FALSE;
  DBUG_ENTER("Event_scheduler::init");
  DBUG_PRINT("enter", ("this=%p", this));
  
  LOCK_SCHEDULER_DATA();
  init_alloc_root(&scheduler_root, MEM_ROOT_BLOCK_SIZE, MEM_ROOT_PREALLOC);
  for (;i < COND_LAST; i++)
    if (pthread_cond_init(&cond_vars[i], NULL))
    {
      sql_print_error("SCHEDULER: Unable to initalize conditions");
      ret= TRUE;
      goto end;
    }
  state= INITIALIZED;
end:
  UNLOCK_SCHEDULER_DATA();
  DBUG_RETURN(ret);
}


/*
  Frees all memory allocated by the scheduler object.

  SYNOPSIS
    Event_scheduler::destroy()
  
  RETURN VALUE
    FALSE  OK
    TRUE   Error
*/

void
Event_scheduler::destroy()
{
  DBUG_ENTER("Event_scheduler");
  LOCK_SCHEDULER_DATA();
  switch (state) {
  case UNINITIALIZED:
    break;
  case INITIALIZED:
    int i;
    for (i= 0; i < COND_LAST; i++)
      pthread_cond_destroy(&cond_vars[i]);
    state= UNINITIALIZED;
    break;
  default:
    sql_print_error("SCHEDULER: Destroying while state is %d", state);
    /* I trust my code but ::safe() > ::sorry() */
    DBUG_ASSERT(0);
    break;
  }
  UNLOCK_SCHEDULER_DATA();

  DBUG_VOID_RETURN;
}


extern pthread_attr_t connection_attrib;


/*
  Starts the event scheduler

  SYNOPSIS
    Event_scheduler::start()

  RETURN VALUE
    FALSE  OK
    TRUE   Error
*/

bool
Event_scheduler::start()
{
  bool ret= FALSE;
  pthread_t th;
  DBUG_ENTER("Event_scheduler::start");

  LOCK_SCHEDULER_DATA();
  /* If already working or starting don't make another attempt */
  DBUG_ASSERT(state == INITIALIZED);
  if (state > INITIALIZED)
  {
    DBUG_PRINT("info", ("scheduler is already running or starting"));
    ret= TRUE;
    goto end;
  }

  /*
    Now if another thread calls start it will bail-out because the branch
    above will be executed. Thus no two or more child threads will be forked.
    If the child thread cannot start for some reason then `state` is set
    to CANTSTART and COND_started is also signaled. In this case we
    set `state` back to INITIALIZED so another attempt to start the scheduler
    can be made.
  */
  state= COMMENCING;
  /* Fork */
  if (pthread_create(&th, &connection_attrib, event_scheduler_thread,
                    (void*)this))
  {
    DBUG_PRINT("error", ("cannot create a new thread"));
    state= INITIALIZED;
    ret= TRUE;
    goto end;
  }

  /*  Wait till the child thread has booted (w/ or wo success) */
  while (!(state == SUSPENDED || state == RUNNING) && state != CANTSTART)
    cond_wait(COND_started_or_stopped, LOCK_scheduler_data);

  /*
    If we cannot start for some reason then don't prohibit further attempts.
    Set back to INITIALIZED.
  */
  if (state == CANTSTART)
  {
    state= INITIALIZED;
    ret= TRUE;
    goto end;
  }

end:
  UNLOCK_SCHEDULER_DATA();
  DBUG_RETURN(ret);
}


/*
  Starts the event scheduler in suspended mode.

  SYNOPSIS
    Event_scheduler::start_suspended()

  RETURN VALUE
    TRUE   OK
    FALSE  Error
*/

bool
Event_scheduler::start_suspended()
{
  DBUG_ENTER("Event_scheduler::start_suspended");
  start_scheduler_suspended= TRUE;
  DBUG_RETURN(start());
}



/*
  Report back that we cannot start. Used for ocasions where
  we can't go into ::run() and have to report externally.

  SYNOPSIS
    Event_scheduler::report_error_during_start()
*/

inline void
Event_scheduler::report_error_during_start()
{
  DBUG_ENTER("Event_scheduler::report_error_during_start");

  LOCK_SCHEDULER_DATA();
  state= CANTSTART;
  DBUG_PRINT("info", ("Sending back COND_started_or_stopped"));
  pthread_cond_signal(&cond_vars[COND_started_or_stopped]);
  UNLOCK_SCHEDULER_DATA();

  DBUG_VOID_RETURN;
}


/*
  The internal loop of the event scheduler

  SYNOPSIS
    Event_scheduler::run()
      thd  Thread

  RETURN VALUE
    FALSE OK
    TRUE  Failure
*/

bool
Event_scheduler::run(THD *thd)
{
  int ret;
  struct timespec abstime;
  DBUG_ENTER("Event_scheduler::run");
  DBUG_PRINT("enter", ("thd=%p", thd));

  LOCK_SCHEDULER_DATA();
  ret= event_queue->load_events_from_db(thd);

  if (!ret)
  {
    thread_id= thd->thread_id;
    state= start_scheduler_suspended? SUSPENDED:RUNNING;
    start_scheduler_suspended= FALSE;
  }
  else 
    state= CANTSTART;

  DBUG_PRINT("info", ("Sending back COND_started_or_stopped"));
  pthread_cond_signal(&cond_vars[COND_started_or_stopped]);
  if (ret)
  {
    UNLOCK_SCHEDULER_DATA();
    DBUG_RETURN(TRUE);
  }
  if (!check_n_suspend_if_needed(thd))
    UNLOCK_SCHEDULER_DATA();

  sql_print_information("SCHEDULER: Manager thread started with id %lu",
                        thd->thread_id);
  abstime.tv_nsec= 0;
  while ((state == SUSPENDED || state == RUNNING))
  {
    Event_timed *et;

    LOCK_SCHEDULER_DATA();
    if (check_n_wait_for_non_empty_queue(thd))
      continue;

    /* On TRUE data is unlocked, go back to the beginning */
    if (check_n_suspend_if_needed(thd))
      continue;

    /* Guaranteed locked here */
    if (state == IN_SHUTDOWN || shutdown_in_progress)
    {
      UNLOCK_SCHEDULER_DATA();
      break;
    }
    DBUG_ASSERT(state == RUNNING);

//    et= (Event_timed *)queue_top(&event_queue->queue);
    et= event_queue->get_top();

    /* Skip disabled events */
    if (et->status != Event_timed::ENABLED)
    {
      /*
        It could be a one-timer scheduled for a time, already in the past when the
        scheduler was suspended.
      */
      sql_print_information("SCHEDULER: Found a disabled event %*s.%*s in the queue",
                            et->dbname.length, et->dbname.str, et->name.length,
                            et->name.str);
      queue_remove(&event_queue->queue, 0);
      /* ToDo: check this again */
      if (et->dropped)
        et->drop(thd);
      delete et;
      UNLOCK_SCHEDULER_DATA();
      continue;
    }
    thd->proc_info= (char *)"Computing";
    DBUG_PRINT("evex manager",("computing time to sleep till next exec"));
    /* Timestamp is in UTC */
    abstime.tv_sec= sec_since_epoch_TIME(&et->execute_at);

    thd->end_time();
    if (abstime.tv_sec > thd->query_start())
    {
      /* Event trigger time is in the future */
      thd->proc_info= (char *)"Sleep";
      DBUG_PRINT("info", ("Going to sleep. Should wakeup after approx %d secs",
                         abstime.tv_sec - thd->query_start()));
      DBUG_PRINT("info", ("Entering condition because waiting for activation"));
      /*
        Use THD::enter_cond()/exit_cond() or we won't be able to kill a
        sleeping thread. Though ::stop() can do it by sending COND_new_work
        an user can't by just issuing 'KILL x'; . In the latter case
        pthread_cond_timedwait() will wait till `abstime`.
        "Sleeping until next time"
      */
      thd->enter_cond(&cond_vars[COND_new_work],LOCK_scheduler_data,"Sleeping");

      pthread_cond_timedwait(&cond_vars[COND_new_work], LOCK_scheduler_data,
                             &abstime);

      DBUG_PRINT("info", ("Manager woke up. state is %d", state));
      /*
        If we get signal we should recalculate the whether it's the right time
        because there could be :
        1. Spurious wake-up
        2. The top of the queue was changed (new one becase of add/drop/replace)
      */
      /* This will do implicit UNLOCK_SCHEDULER_DATA() */
      thd->exit_cond("");
    }
    else
    {
      thd->proc_info= (char *)"Executing";
      /*
        Execute the event. An error may occur if a thread cannot be forked.
        In this case stop  the manager.
        We should enter ::execute_top() with locked LOCK_scheduler_data.
      */
      int ret= execute_top(thd, et);
      UNLOCK_SCHEDULER_DATA();
      if (ret)
        break;
    }
  }

  thd->proc_info= (char *)"Cleaning";

  LOCK_SCHEDULER_DATA();
  /*
    It's possible that a user has used (SQL)COM_KILL. Hence set the appropriate
    state because it is only set by ::stop().
  */
  if (state != IN_SHUTDOWN)
  {
    DBUG_PRINT("info", ("We got KILL but the but not from ::stop()"));
    state= IN_SHUTDOWN;
  }
  UNLOCK_SCHEDULER_DATA();

  sql_print_information("SCHEDULER: Shutting down");

  thd->proc_info= (char *)"Cleaning queue";
  clean_memory(thd);
  THD_CHECK_SENTRY(thd);

  /* free mamager_root memory but don't destroy the root */
  thd->proc_info= (char *)"Cleaning memory root";
  free_root(&scheduler_root, MYF(0));
  THD_CHECK_SENTRY(thd);

  /*
    We notify the waiting thread which shutdowns us that we have cleaned.
    There are few more instructions to be executed in this pthread but
    they don't affect manager structures thus it's safe to signal already
    at this point.
  */
  LOCK_SCHEDULER_DATA();
  thd->proc_info= (char *)"Sending shutdown signal";
  DBUG_PRINT("info", ("Sending COND_started_or_stopped"));
  if (state == IN_SHUTDOWN)
    pthread_cond_signal(&cond_vars[COND_started_or_stopped]);

  state= INITIALIZED;
  /*
    We set it here because ::run() can stop not only because of ::stop()
    call but also because of `KILL x`
  */
  thread_id= 0;
  sql_print_information("SCHEDULER: Stopped");
  UNLOCK_SCHEDULER_DATA();

  /* We have modified, we set back */
  thd->query= NULL;
  thd->query_length= 0;

  DBUG_RETURN(FALSE);
}


/*
  Executes the top element of the queue. Auxiliary method for ::run().

  SYNOPSIS
    Event_scheduler::execute_top()

  RETURN VALUE
    FALSE OK
    TRUE  Failure

  NOTE
    NO locking is done. EXPECTED is that the caller should have locked
    the queue (w/ LOCK_scheduler_data).
*/

bool
Event_scheduler::execute_top(THD *thd, Event_timed *et)
{
  int spawn_ret_code;
  bool ret= FALSE;
  DBUG_ENTER("Event_scheduler::execute_top");
  DBUG_PRINT("enter", ("thd=%p", thd));

  /* Is it good idea to pass a stack address ?*/
  Worker_thread_param param(et);

  pthread_mutex_lock(&param.LOCK_started);
  /* 
    We don't lock LOCK_scheduler_data fpr workers_increment() because it's a
    pre-requisite for calling the current_method.
  */
  switch ((spawn_ret_code= et->spawn_now(event_worker_thread, &param))) {
  case EVENT_EXEC_CANT_FORK:
    /* 
      We don't lock LOCK_scheduler_data here because it's a pre-requisite
      for calling the current_method.
    */
    sql_print_error("SCHEDULER: Problem while trying to create a thread");
    ret= TRUE;
    break;
  case EVENT_EXEC_ALREADY_EXEC:
    /* 
      We don't lock LOCK_scheduler_data here because it's a pre-requisite
      for calling the current_method.
    */
    sql_print_information("SCHEDULER: %s.%s in execution. Skip this time.",
                          et->dbname.str, et->name.str);
    if ((et->flags & EVENT_EXEC_NO_MORE) || et->status == Event_timed::DISABLED)
      event_queue->remove_top();
    else
      event_queue->top_changed();
    break;
  default:
    DBUG_ASSERT(!spawn_ret_code);
    if ((et->flags & EVENT_EXEC_NO_MORE) || et->status == Event_timed::DISABLED)
      event_queue->remove_top();
    else
      event_queue->top_changed();
    /* 
      We don't lock LOCK_scheduler_data here because it's a pre-requisite
      for calling the current_method.
    */
    if (likely(!spawn_ret_code))
    {
      /* Wait the forked thread to start */
      do {
        pthread_cond_wait(&param.COND_started, &param.LOCK_started);
      } while (!param.started);
    }
    /*
      param was allocated on the stack so no explicit delete as well as
      in this moment it's no more used in the spawned thread so it's safe
      to be deleted.
    */
    break;
  }
  pthread_mutex_unlock(&param.LOCK_started);
  /* `param` is on the stack and will be destructed by the compiler */

  DBUG_RETURN(ret);
}


/*
  Cleans the scheduler's queue. Auxiliary method for ::run().

  SYNOPSIS
    Event_scheduler::clean_queue()
      thd  Thread
*/

void
Event_scheduler::clean_memory(THD *thd)
{
  CHARSET_INFO *scs= system_charset_info;
  uint i;
  DBUG_ENTER("Event_scheduler::clean_queue");
  DBUG_PRINT("enter", ("thd=%p", thd));

  LOCK_SCHEDULER_DATA();
  stop_all_running_events(thd);
  UNLOCK_SCHEDULER_DATA();

  sql_print_information("SCHEDULER: Emptying the queue");

  event_queue->empty_queue();

  DBUG_VOID_RETURN;
}


/*
  Stops all running events

  SYNOPSIS
    Event_scheduler::stop_all_running_events()
      thd  Thread

  NOTE
    LOCK_scheduler data must be acquired prior to call to this method
*/

void
Event_scheduler::stop_all_running_events(THD *thd)
{
  CHARSET_INFO *scs= system_charset_info;
  uint i;
  DYNAMIC_ARRAY running_threads;
  THD *tmp;
  DBUG_ENTER("Event_scheduler::stop_all_running_events");
  DBUG_PRINT("enter", ("workers_count=%d", workers_count()));

  my_init_dynamic_array(&running_threads, sizeof(ulong), 10, 10);

  bool had_super= FALSE;
  VOID(pthread_mutex_lock(&LOCK_thread_count)); // For unlink from list
  I_List_iterator<THD> it(threads);
  while ((tmp=it++))
  {
    if (tmp->command == COM_DAEMON)
      continue;
    if (tmp->system_thread == SYSTEM_THREAD_EVENT_WORKER)
      push_dynamic(&running_threads, (gptr) &tmp->thread_id);
  }
  VOID(pthread_mutex_unlock(&LOCK_thread_count));

  /* We need temporarily SUPER_ACL to be able to kill our offsprings */
  if (!(thd->security_ctx->master_access & SUPER_ACL))
    thd->security_ctx->master_access|= SUPER_ACL;
  else
    had_super= TRUE;

  char tmp_buff[10*STRING_BUFFER_USUAL_SIZE];
  char int_buff[STRING_BUFFER_USUAL_SIZE];
  String tmp_string(tmp_buff, sizeof(tmp_buff), scs);
  String int_string(int_buff, sizeof(int_buff), scs);
  tmp_string.length(0);

  for (i= 0; i < running_threads.elements; ++i)
  {
    int ret;
    ulong thd_id= *dynamic_element(&running_threads, i, ulong*);

    int_string.set((longlong) thd_id,scs);
    tmp_string.append(int_string);
    if (i < running_threads.elements - 1)
      tmp_string.append(' ');

    if ((ret= kill_one_thread(thd, thd_id, FALSE)))
    {
      sql_print_error("SCHEDULER: Error killing %lu code=%d", thd_id, ret);
      break;
    }
  }
  if (running_threads.elements)
    sql_print_information("SCHEDULER: Killing workers :%s", tmp_string.c_ptr());

  if (!had_super)
    thd->security_ctx->master_access &= ~SUPER_ACL;

  delete_dynamic(&running_threads);

  sql_print_information("SCHEDULER: Waiting for worker threads to finish");

  while (workers_count())
    my_sleep(100000);

  DBUG_VOID_RETURN;
}


/*
  Stops the event scheduler

  SYNOPSIS
    Event_scheduler::stop()

  RETURN VALUE
    OP_OK           OK
    OP_CANT_KILL    Error during stopping of manager thread
    OP_NOT_RUNNING  Manager not working

  NOTE
    The caller must have acquited LOCK_scheduler_data.
*/

int
Event_scheduler::stop()
{
  THD *thd= current_thd;
  DBUG_ENTER("Event_scheduler::stop");
  DBUG_PRINT("enter", ("thd=%p", current_thd));

  LOCK_SCHEDULER_DATA();
  if (!(state == SUSPENDED || state == RUNNING))
  {
    /*
      One situation to be here is if there was a start that forked a new
      thread but the new thread did not acquire yet LOCK_scheduler_data.
      Hence, in this case return an error.
    */
    DBUG_PRINT("info", ("manager not running but %d. doing nothing", state));
    UNLOCK_SCHEDULER_DATA();
    DBUG_RETURN(OP_NOT_RUNNING);
  }
  state= IN_SHUTDOWN;

  DBUG_PRINT("info", ("Manager thread has id %d", thread_id));
  sql_print_information("SCHEDULER: Killing manager thread %lu", thread_id);
  
  /* 
    Sending the COND_new_work to ::run() is a way to get this working without
    race conditions. If we use kill_one_thread() it will call THD::awake() and
    because in ::run() both THD::enter_cond()/::exit_cond() are used,
    THD::awake() will try to lock LOCK_scheduler_data. If we UNLOCK it before,
    then the pthread_cond_signal(COND_started_or_stopped) could be signaled in
    ::run() and we can miss the signal before we relock. A way is to use
    another mutex for this shutdown procedure but better not.
  */
  pthread_cond_signal(&cond_vars[COND_new_work]);
  /* Or we are suspended - then we should wake up */
  pthread_cond_signal(&cond_vars[COND_suspend_or_resume]);

  /* Guarantee we don't catch spurious signals */
  sql_print_information("SCHEDULER: Waiting the manager thread to reply");
  while (state != INITIALIZED)
  {
    DBUG_PRINT("info", ("Waiting for COND_started_or_stopped from the manager "
                        "thread.  Current value of state is %d . "
                        "workers count=%d", state, workers_count()));
    cond_wait(COND_started_or_stopped, LOCK_scheduler_data);
  }
  DBUG_PRINT("info", ("Manager thread has cleaned up. Set state to INIT"));
  UNLOCK_SCHEDULER_DATA();

  DBUG_RETURN(OP_OK);
}


/*
  Suspends or resumes the scheduler.
  SUSPEND - it won't execute any event till resumed.
  RESUME - it will resume if suspended.

  SYNOPSIS
    Event_scheduler::suspend_or_resume()

  RETURN VALUE
    OP_OK  OK
*/

int
Event_scheduler::suspend_or_resume(
              enum Event_scheduler::enum_suspend_or_resume action)
{
  DBUG_ENTER("Event_scheduler::suspend_or_resume");
  DBUG_PRINT("enter", ("action=%d", action));

  LOCK_SCHEDULER_DATA();

  if ((action == SUSPEND && state == SUSPENDED) ||
      (action == RESUME  && state == RUNNING))
  {
    DBUG_PRINT("info", ("Either trying to suspend suspended or resume "
               "running scheduler. Doing nothing."));
  }
  else
  {
    /* Wake the main thread up if he is asleep */
    DBUG_PRINT("info", ("Sending signal"));
    if (action==SUSPEND)
    {
      state= SUSPENDED;
      pthread_cond_signal(&cond_vars[COND_new_work]);
    }
    else
    {
      state= RUNNING;
      pthread_cond_signal(&cond_vars[COND_suspend_or_resume]);
    }
    DBUG_PRINT("info", ("Waiting on COND_suspend_or_resume"));
    cond_wait(COND_suspend_or_resume, LOCK_scheduler_data);
    DBUG_PRINT("info", ("Got response"));
  }
  UNLOCK_SCHEDULER_DATA();
  DBUG_RETURN(OP_OK);
}


/*
  Returns the number of executing events.

  SYNOPSIS
    Event_scheduler::workers_count()
*/

uint
Event_scheduler::workers_count()
{
  THD *tmp;
  uint count= 0;
  
  DBUG_ENTER("Event_scheduler::workers_count");
  VOID(pthread_mutex_lock(&LOCK_thread_count)); // For unlink from list
  I_List_iterator<THD> it(threads);
  while ((tmp=it++))
  {
    if (tmp->command == COM_DAEMON)
      continue;
    if (tmp->system_thread == SYSTEM_THREAD_EVENT_WORKER)
      ++count;
  }
  VOID(pthread_mutex_unlock(&LOCK_thread_count));
  DBUG_PRINT("exit", ("%d", count));
  DBUG_RETURN(count);
}


/*
  Checks and suspends if needed

  SYNOPSIS
    Event_scheduler::check_n_suspend_if_needed()
      thd  Thread

  RETURN VALUE
    FALSE  Not suspended, we haven't slept
    TRUE   We were suspended. LOCK_scheduler_data is unlocked.

  NOTE
    The caller should have locked LOCK_scheduler_data!
    The mutex will be unlocked in case this function returns TRUE
*/

bool
Event_scheduler::check_n_suspend_if_needed(THD *thd)
{
  bool was_suspended= FALSE;
  DBUG_ENTER("Event_scheduler::check_n_suspend_if_needed");
  if (thd->killed && !shutdown_in_progress)
  {
    state= SUSPENDED;
    thd->killed= THD::NOT_KILLED;
  }
  if (state == SUSPENDED)
  {
    thd->enter_cond(&cond_vars[COND_suspend_or_resume], LOCK_scheduler_data,
                    "Suspended");
    /* Send back signal to the thread that asked us to suspend operations */
    pthread_cond_signal(&cond_vars[COND_suspend_or_resume]);
    sql_print_information("SCHEDULER: Suspending operations");
    was_suspended= TRUE;
  }
  while (state == SUSPENDED)
  {
    cond_wait(COND_suspend_or_resume, LOCK_scheduler_data);
    DBUG_PRINT("info", ("Woke up after waiting on COND_suspend_or_resume"));
    if (state != SUSPENDED)
    {
      pthread_cond_signal(&cond_vars[COND_suspend_or_resume]);
      sql_print_information("SCHEDULER: Resuming operations");
    }
  }
  if (was_suspended)
  {
    event_queue->recalculate_queue(thd);
    /* This will implicitly unlock LOCK_scheduler_data */
    thd->exit_cond("");
  }
  DBUG_RETURN(was_suspended);
}


/*
  Checks for empty queue and waits till new element gets in

  SYNOPSIS
    Event_scheduler::check_n_wait_for_non_empty_queue()
      thd  Thread

  RETURN VALUE
    FALSE  Did not wait - LOCK_scheduler_data still locked.
    TRUE   Waited - LOCK_scheduler_data unlocked.

  NOTE
    The caller should have locked LOCK_scheduler_data!
*/

bool
Event_scheduler::check_n_wait_for_non_empty_queue(THD *thd)
{
  bool slept= FALSE;
  DBUG_ENTER("Event_scheduler::check_n_wait_for_non_empty_queue");
  DBUG_PRINT("enter", ("q.elements=%lu state=%s",
             event_queue->events_count_no_lock(), states_names[state]));
  
  if (!event_queue->events_count_no_lock())
    thd->enter_cond(&cond_vars[COND_new_work], LOCK_scheduler_data,
                    "Empty queue, sleeping");

  /* Wait in a loop protecting against catching spurious signals */
  while (!event_queue->events_count_no_lock() && state == RUNNING)
  {
    slept= TRUE;
    DBUG_PRINT("info", ("Entering condition because of empty queue"));
    cond_wait(COND_new_work, LOCK_scheduler_data);
    DBUG_PRINT("info", ("Manager woke up. Hope we have events now. state=%d",
               state));
    /*
      exit_cond does implicit mutex_UNLOCK, we needed it locked if
      1. we loop again
      2. end the current loop and start doing calculations
    */
  }
  if (slept)
    thd->exit_cond("");

  DBUG_PRINT("exit", ("q.elements=%lu state=%s thd->killed=%d",
             event_queue->events_count_no_lock(), states_names[state], thd->killed));

  DBUG_RETURN(slept);
}


/*
  Returns the current state of the scheduler

  SYNOPSIS
    Event_scheduler::get_state()
*/

enum Event_scheduler::enum_state
Event_scheduler::get_state()
{
  enum Event_scheduler::enum_state ret;
  DBUG_ENTER("Event_scheduler::get_state");
  /* lock_data & unlock_data are not static */
  pthread_mutex_lock(singleton->LOCK_scheduler_data);
  ret= singleton->state;
  pthread_mutex_unlock(singleton->LOCK_scheduler_data);
  DBUG_RETURN(ret);
}


/*
  Returns whether the scheduler was initialized.

  SYNOPSIS
    Event_scheduler::initialized()

  RETURN VALUE
    FALSE  Was not initialized so far
    TRUE   Was initialized
*/

bool
Event_scheduler::initialized()
{
  DBUG_ENTER("Event_scheduler::initialized");
  DBUG_RETURN(Event_scheduler::get_state() != UNINITIALIZED);
}




/*
  Dumps some data about the internal status of the scheduler.

  SYNOPSIS
    Event_scheduler::dump_internal_status()
      thd      THD

  RETURN VALUE
    0  OK
    1  Error
*/

int
Event_scheduler::dump_internal_status(THD *thd)
{
  DBUG_ENTER("dump_internal_status");
#ifndef DBUG_OFF
  CHARSET_INFO *scs= system_charset_info;
  Protocol *protocol= thd->protocol;
  List<Item> field_list;
  int ret;
  char tmp_buff[5*STRING_BUFFER_USUAL_SIZE];
  char int_buff[STRING_BUFFER_USUAL_SIZE];
  String tmp_string(tmp_buff, sizeof(tmp_buff), scs);
  String int_string(int_buff, sizeof(int_buff), scs);
  tmp_string.length(0);
  int_string.length(0);

  field_list.push_back(new Item_empty_string("Name", 20));
  field_list.push_back(new Item_empty_string("Value",20));
  if (protocol->send_fields(&field_list, Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    DBUG_RETURN(1);

  protocol->prepare_for_resend();
  protocol->store(STRING_WITH_LEN("state"), scs);
  protocol->store(states_names[singleton->state].str,
                  states_names[singleton->state].length,
                  scs);

  ret= protocol->write();
  /*
    If not initialized - don't show anything else. get_instance()
    will otherwise implicitly initialize it. We don't want that.
  */
  if (singleton->state >= INITIALIZED)
  {
    /* last locked at*/
    /* 
      The first thing to do, or get_instance() will overwrite the values.
      mutex_last_locked_at_line / mutex_last_unlocked_at_line
    */
    protocol->prepare_for_resend();
    protocol->store(STRING_WITH_LEN("last locked at"), scs);
    tmp_string.length(scs->cset->snprintf(scs, (char*) tmp_string.ptr(),
                                       tmp_string.alloced_length(), "%s::%d",
                                       singleton->mutex_last_locked_in_func,
                                       singleton->mutex_last_locked_at_line));
    protocol->store(&tmp_string);
    ret= protocol->write();

    /* last unlocked at*/
    protocol->prepare_for_resend();
    protocol->store(STRING_WITH_LEN("last unlocked at"), scs);
    tmp_string.length(scs->cset->snprintf(scs, (char*) tmp_string.ptr(),
                                       tmp_string.alloced_length(), "%s::%d",
                                       singleton->mutex_last_unlocked_in_func,
                                       singleton->mutex_last_unlocked_at_line));
    protocol->store(&tmp_string);
    ret= protocol->write();

    /* waiting on */
    protocol->prepare_for_resend();
    protocol->store(STRING_WITH_LEN("waiting on condition"), scs);
    tmp_string.length(scs->cset->
                    snprintf(scs, (char*) tmp_string.ptr(),
                             tmp_string.alloced_length(), "%s",
                             (singleton->cond_waiting_on != COND_NONE) ?
                               cond_vars_names[singleton->cond_waiting_on]:
                               "NONE"));
    protocol->store(&tmp_string);
    ret= protocol->write();

    Event_scheduler *scheduler= get_instance();

    /* workers_count */
    protocol->prepare_for_resend();
    protocol->store(STRING_WITH_LEN("workers_count"), scs);
    int_string.set((longlong) scheduler->workers_count(), scs);
    protocol->store(&int_string);
    ret= protocol->write();

    /* queue.elements */
    protocol->prepare_for_resend();
    protocol->store(STRING_WITH_LEN("queue.elements"), scs);
    int_string.set((longlong) scheduler->event_queue->events_count_no_lock(), scs);
    protocol->store(&int_string);
    ret= protocol->write();

    /* scheduler_data_locked */
    protocol->prepare_for_resend();
    protocol->store(STRING_WITH_LEN("scheduler data locked"), scs);
    int_string.set((longlong) scheduler->mutex_scheduler_data_locked, scs);
    protocol->store(&int_string);
    ret= protocol->write();
  }
  send_eof(thd);
#endif
  DBUG_RETURN(0);
}


/*
  Wrapper for pthread_mutex_lock

  SYNOPSIS
    Event_scheduler::lock_data()
      mutex Mutex to lock
      line  The line number on which the lock is done

  RETURN VALUE
    Error code of pthread_mutex_lock()
*/

void
Event_scheduler::lock_data(const char *func, uint line)
{
  DBUG_ENTER("Event_scheduler::lock_mutex");
  DBUG_PRINT("enter", ("mutex_lock=%p func=%s line=%u",
             &LOCK_scheduler_data, func, line));
  pthread_mutex_lock(LOCK_scheduler_data);
  mutex_last_locked_in_func= func;
  mutex_last_locked_at_line= line;
  mutex_scheduler_data_locked= TRUE;
  DBUG_VOID_RETURN;
}


/*
  Wrapper for pthread_mutex_unlock

  SYNOPSIS
    Event_scheduler::unlock_data()
      mutex Mutex to unlock
      line  The line number on which the unlock is done
*/

void
Event_scheduler::unlock_data(const char *func, uint line)
{
  DBUG_ENTER("Event_scheduler::UNLOCK_mutex");
  DBUG_PRINT("enter", ("mutex_unlock=%p func=%s line=%u",
             LOCK_scheduler_data, func, line));
  mutex_last_unlocked_at_line= line;
  mutex_scheduler_data_locked= FALSE;
  mutex_last_unlocked_in_func= func;
  pthread_mutex_unlock(LOCK_scheduler_data);
  DBUG_VOID_RETURN;
}


/*
  Wrapper for pthread_cond_wait

  SYNOPSIS
    Event_scheduler::cond_wait()
      cond   Conditional to wait for
      mutex  Mutex of the conditional

  RETURN VALUE
    Error code of pthread_cond_wait()
*/

int
Event_scheduler::cond_wait(int cond, pthread_mutex_t *mutex)
{
  int ret;
  DBUG_ENTER("Event_scheduler::cond_wait");
  DBUG_PRINT("enter", ("cond=%s mutex=%p", cond_vars_names[cond], mutex));
  ret= pthread_cond_wait(&cond_vars[cond_waiting_on=cond], mutex);
  cond_waiting_on= COND_NONE;
  DBUG_RETURN(ret);
}


/*
  Signals the main scheduler thread that the queue has changed
  its state.

  SYNOPSIS
    Event_scheduler::queue_changed()
*/

void
Event_scheduler::queue_changed()
{
  DBUG_ENTER("Event_scheduler::queue_changed");
  DBUG_PRINT("info", ("Sending COND_new_work"));
  pthread_cond_signal(&cond_vars[COND_new_work]);
  DBUG_VOID_RETURN;
}


/*
  Inits mutexes.

  SYNOPSIS
    Event_scheduler::init_mutexes()
*/

void
Event_scheduler::init_mutexes()
{
  pthread_mutex_init(singleton->LOCK_scheduler_data, MY_MUTEX_INIT_FAST);
}


/*
  Destroys mutexes.

  SYNOPSIS
    Event_queue::destroy_mutexes()
*/

void
Event_scheduler::destroy_mutexes()
{
  pthread_mutex_destroy(singleton->LOCK_scheduler_data);
}

/*
 * This file is distributed as part of the MariaDB Corporation MaxScale.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright MariaDB Corporation Ab 2013-2014
 */
#include <my_config.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <router.h>
#include <dbshard.h>
#include <secrets.h>
#include <mysql.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <query_classifier.h>
#include <dcb.h>
#include <spinlock.h>
#include <modinfo.h>
#include <modutil.h>
#include <mysql_client_server_protocol.h>

MODULE_INFO 	info = {
	MODULE_API_ROUTER,
	MODULE_BETA_RELEASE,
	ROUTER_VERSION,
	"A database sharding router for simple sharding"
};
#if defined(SS_DEBUG)
#  include <mysql_client_server_protocol.h>
#endif

/** Defined in log_manager.cc */
extern int            lm_enabled_logfiles_bitmask;
extern size_t         log_ses_count[];
extern __thread log_info_t tls_log_info;
/**
 * @file readwritesplit.c	The entry points for the read/write query splitting
 * router module.
 *
 * This file contains the entry points that comprise the API to the read write
 * query splitting router.
 * @verbatim
 * Revision History
 *
 * Date		Who			Description
 * 01/07/2013	Vilho Raatikka		Initial implementation
 * 15/07/2013	Massimiliano Pinto	Added clientReply
 *					from master only in case of session change
 * 17/07/2013	Massimiliano Pinto	clientReply is now used by mysql_backend
 *					for all reply situations
 * 18/07/2013	Massimiliano Pinto	routeQuery now handles COM_QUIT
 *					as QUERY_TYPE_SESSION_WRITE
 * 17/07/2014	Massimiliano Pinto	Server connection counter is updated in closeSession
 *
 * @endverbatim
 */

static char *version_str = "V1.0.2";

static	ROUTER* createInstance(SERVICE *service, char **options);
static	void*   newSession(ROUTER *instance, SESSION *session);
static	void    closeSession(ROUTER *instance, void *session);
static	void    freeSession(ROUTER *instance, void *session);
static	int     routeQuery(ROUTER *instance, void *session, GWBUF *queue);
static	void    diagnostic(ROUTER *instance, DCB *dcb);

static  void	clientReply(
        ROUTER* instance,
        void*   router_session,
        GWBUF*  queue,
        DCB*    backend_dcb);

static  void           handleError(
        ROUTER*        instance,
        void*          router_session,
        GWBUF*         errmsgbuf,
        DCB*           backend_dcb,
        error_action_t action,
        bool*          succp);

static void print_error_packet(ROUTER_CLIENT_SES* rses, GWBUF* buf, DCB* dcb);
static int  router_get_servercount(ROUTER_INSTANCE* router);
static backend_ref_t* get_bref_from_dcb(ROUTER_CLIENT_SES* rses, DCB* dcb);

static route_target_t get_shard_route_target (
	skygw_query_type_t qtype,
	bool               trx_active,
	target_t           use_sql_variables_in,
	HINT*              hint);
#if 0
static backend_ref_t* check_candidate_bref(
	backend_ref_t* candidate_bref,
	backend_ref_t* new_bref,
	select_criteria_t sc);
#endif

static  uint8_t getCapabilities (ROUTER* inst, void* router_session);

#if defined(NOT_USED)
static bool router_option_configured(
        ROUTER_INSTANCE* router,
        const char*      optionstr,
        void*            data);
#endif

#if defined(PREP_STMT_CACHING)
static prep_stmt_t* prep_stmt_init(prep_stmt_type_t type, void* id);
static void         prep_stmt_done(prep_stmt_t* pstmt);
#endif /*< PREP_STMT_CACHING */

bool parse_db_ignore_list(ROUTER_INSTANCE* router,char* param);

int bref_cmp_global_conn(
        const void* bref1,
        const void* bref2);

int bref_cmp_router_conn(
        const void* bref1,
        const void* bref2);

int bref_cmp_behind_master(
        const void* bref1,
        const void* bref2);

int bref_cmp_current_load(
        const void* bref1,
        const void* bref2);

static bool connect_backend_servers(
        backend_ref_t*     backend_ref,
        int                router_nservers,
        SESSION*           session,
        ROUTER_INSTANCE*   router);

static bool get_shard_dcb(
        DCB**              dcb,
        ROUTER_CLIENT_SES* rses,
        char*              name);
#if 0
static void rwsplit_process_router_options(
        ROUTER_INSTANCE* router,
        char**           options);
#endif

static ROUTER_OBJECT MyObject = {
        createInstance,
        newSession,
        closeSession,
        freeSession,
        routeQuery,
        diagnostic,
        clientReply,
	handleError,
        getCapabilities
};
static bool rses_begin_locked_router_action(
        ROUTER_CLIENT_SES* rses);

static void rses_end_locked_router_action(
        ROUTER_CLIENT_SES* rses);

static void mysql_sescmd_done(
	mysql_sescmd_t* sescmd);

static mysql_sescmd_t* mysql_sescmd_init (
	rses_property_t*   rses_prop,
	GWBUF*             sescmd_buf,
        unsigned char      packet_type,
	ROUTER_CLIENT_SES* rses);

static rses_property_t* mysql_sescmd_get_property(
	mysql_sescmd_t* scmd);

static rses_property_t* rses_property_init(
	rses_property_type_t prop_type);

static void rses_property_add(
	ROUTER_CLIENT_SES* rses,
	rses_property_t*   prop);

static void rses_property_done(
	rses_property_t* prop);

static mysql_sescmd_t* rses_property_get_sescmd(
        rses_property_t* prop);

static bool execute_sescmd_history(backend_ref_t* bref);

static bool execute_sescmd_in_backend(
        backend_ref_t* backend_ref);

static void sescmd_cursor_reset(sescmd_cursor_t* scur);

static bool sescmd_cursor_history_empty(sescmd_cursor_t* scur);

static void sescmd_cursor_set_active(
        sescmd_cursor_t* sescmd_cursor,
        bool             value);

static bool sescmd_cursor_is_active(
	sescmd_cursor_t* sescmd_cursor);

static GWBUF* sescmd_cursor_clone_querybuf(
	sescmd_cursor_t* scur);

static mysql_sescmd_t* sescmd_cursor_get_command(
	sescmd_cursor_t* scur);

static bool sescmd_cursor_next(
	sescmd_cursor_t* scur);

static GWBUF* sescmd_cursor_process_replies(GWBUF* replybuf, backend_ref_t* bref);

static void tracelog_routed_query(
        ROUTER_CLIENT_SES* rses,
        char*              funcname,
        backend_ref_t*     bref,
        GWBUF*             buf);

static void dbshard_process_router_options(
        ROUTER_INSTANCE* router,
        char**           options);


static bool route_session_write(
        ROUTER_CLIENT_SES* router_client_ses,
        GWBUF*             querybuf,
        ROUTER_INSTANCE*   inst,
        unsigned char      packet_type,
        skygw_query_type_t qtype);

static void refreshInstance(
        ROUTER_INSTANCE*  router,
        CONFIG_PARAMETER* param);

static void bref_clear_state(backend_ref_t* bref, bref_state_t state);
static void bref_set_state(backend_ref_t*   bref, bref_state_t state);
static sescmd_cursor_t* backend_ref_get_sescmd_cursor (backend_ref_t* bref);
static int  router_handle_state_switch(DCB* dcb, DCB_REASON reason, void* data);
static bool handle_error_new_connection(
        ROUTER_INSTANCE*   inst,
        ROUTER_CLIENT_SES* rses,
        DCB*               backend_dcb,
        GWBUF*             errmsg);
static void handle_error_reply_client(
	SESSION*           ses, 
	ROUTER_CLIENT_SES* rses, 
	DCB*               backend_dcb,
	GWBUF*             errmsg);


static SPINLOCK	        instlock;
static ROUTER_INSTANCE* instances;

static int hashkeyfun(void* key);
static int hashcmpfun (void *, void *);

static bool change_current_db(
	ROUTER_INSTANCE*   inst,
	ROUTER_CLIENT_SES* rses,
	GWBUF*             buf);


static int hashkeyfun(void* key)
{
  if(key == NULL){
    return 0;
  }
  unsigned int hash = 0,c = 0;
  char* ptr = (char*)key;
  while((c = *ptr++)){
    hash = c + (hash << 6) + (hash << 16) - hash;
  }
  return *(int *)key;
}

static int hashcmpfun(
		  void* v1,
		  void* v2)
{
  char* i1 = (char*) v1;
  char* i2 = (char*) v2;

  return strcmp(i1,i2);
}

static void* hstrdup(void* fval)
{
  char* str = (char*)fval;
  return strdup(str);
}


static void* hfree(void* fval)
{
  free (fval);
  return NULL;
}


/**
 * Updates the hashtable with the database names and where to find them, adding 
 * new and removing obsolete pairs.
 * @param backends Backends to query for database names
 * @param hashtable Hashtable to use
 * @return True if all database and server names were successfully retrieved 
 * otherwise false
 */
bool update_dbnames_hash(BACKEND** backends, HASHTABLE* hashtable)
{
	const unsigned int connect_timeout = 15;
	const unsigned int read_timeout = 10;
	bool rval = true;
	SERVER* server;
	int i, rc, numfields;

	for(i = 0;backends[i];i++)
	{
		MYSQL* handle = mysql_init(NULL);
		MYSQL_RES* result = NULL;
		MYSQL_ROW row;
		char *user,*pwd = NULL;

		if(handle == NULL)
		{
			LOGIF(LE, (skygw_log_write_flush(
					LOGFILE_ERROR,
					"Error: failed to initialize "
					"MySQL handle.")));
			return false;
		}
		rc = 0;	
		rc |= mysql_options(handle, 
				    MYSQL_OPT_CONNECT_TIMEOUT, 
				    (void *)&connect_timeout);
		rc |= mysql_options(handle, 
				    MYSQL_OPT_READ_TIMEOUT, 
				    (void *)&read_timeout);
		if(rc != 0)
		{
			LOGIF(LE, (skygw_log_write_flush(
				LOGFILE_ERROR,
				"Error: failed to set MySQL connection options.")));				
			mysql_close(handle);
			rval = false;
			continue;
		}
		server = backends[i]->backend_server;

		ss_dassert(server != NULL);

		if(server->monuser == NULL || server->monpw == NULL)
		{
			LOGIF(LE, (skygw_log_write_flush(
						 LOGFILE_ERROR,
						"Error: no username or password "
						"defined for server '%s'.",
						server->unique_name)));	
			rval = false;
			goto cleanup;
		}
		/** Plain-text password used for authentication for now */
		user = server->monuser;
		pwd = server->monpw; 

		if (mysql_real_connect(handle,
					server->name,
					user,
					pwd,
					NULL,
					server->port,
					NULL,
					0) == NULL)
			{
				LOGIF(LE, (skygw_log_write_flush(
						LOGFILE_ERROR,
						"Error: failed to connect to backend "
						"server '%s': %d %s",
						server->name,
						mysql_errno(handle),
						mysql_error(handle))));	
			rval = false;
			goto cleanup;
		}
		/**
		 * The server was successfully connected to, proceed to query for database names
		 */
	
		if((result = mysql_list_dbs(handle,NULL)) == NULL)
		{
			LOGIF(LE, (skygw_log_write_flush(LOGFILE_ERROR,
							"Error: failed to retrieve databases from backend "
							"server '%s': %d %s",
							server->name,
							mysql_errno(handle),
							mysql_error(handle))));
			goto cleanup;
		}
		numfields = mysql_num_fields(result);

		if(numfields < 1)
		{
			LOGIF(LT, (skygw_log_write_flush(
					LOGFILE_TRACE,
					"Backend '%s' has no databases.",
					server->name)));
			goto cleanup;
		}
		/**
		 * Walk through the list of databases in this backend
		 * and insert them into the hashtable. If the value is already in the hashtable
		 * but the backend isn't in the list of backends it is replaced with the first found backend.
		 */
		while((row = mysql_fetch_row(result)))
		{
			unsigned long *lengths;
			char *dbnm = NULL,*servnm = NULL;
			
			lengths = mysql_fetch_lengths(result);

			/**
			 * Default databases to ignore, disable for now
			 */
#ifdef NO_DEFAULT_DATABASES
			if(strncmp(row[0],"information_schema",lengths[0]) == 0 ||
			   strncmp(row[0],"performance_schema",lengths[0]) == 0 ||
			   strncmp(row[0],"mysql",lengths[0]) == 0 ||
			   strncmp(row[0],"mysqlslap",lengths[0]) == 0 ||
			   strncmp(row[0],"test",lengths[0]) == 0){
				continue;
			}
#endif
			dbnm = (char*)calloc(lengths[0] + 1,sizeof(char));
			memcpy(dbnm,row[0],lengths[0]);
			servnm = strdup(server->unique_name);
			
			if(hashtable_add(hashtable,dbnm,servnm) == 0)
			{
				char* srvname;

				if((srvname = hashtable_fetch(hashtable,dbnm)) == NULL)
			    {
					LOGIF(LE, (skygw_log_write_flush(
							LOGFILE_ERROR,
							"Error: failed to insert values into hashtable.")));
				}
				else
				{
					LOGIF(LE, (skygw_log_write_flush(
							LOGFILE_ERROR,
							"Warning : conflicting "
							"databases found. "
							"Both \"%s\" and \"%s\" "
							"have a database \"%s\".",
							server->unique_name,
							srvname,
							dbnm)));
				}
				
				char* old_backend = (char*)hashtable_fetch(hashtable,dbnm);
				int j;
				bool is_alive = false;

				for(j = 0;backends[j];j++)
				{
					/**
					 * See if the old backend is still
					 * alive. If not then update
					 * the hashtable with the current backend's name.
					 */
					if(strcmp(server->unique_name,old_backend) == 0 && 
					   SERVER_IS_RUNNING(server))
					{
						is_alive = true;
						break;
					}
				}
					
				if(!is_alive)
				{
					hashtable_delete(hashtable,dbnm);
					
					if(hashtable_add(hashtable,dbnm,servnm))
					{
						LOGIF(LT, (skygw_log_write_flush(
									LOGFILE_TRACE,
									"Updated the backend of database '%s' to '%s'.",
									dbnm,
									servnm)));
					}
					else
					{
						LOGIF(LE, (skygw_log_write_flush(
									LOGFILE_ERROR,
									"Error: failed to insert values into hashtable.")));
					}
				}
			} /*< hashtable_add failed */
		} /*< while */

cleanup:		
		if(result)
		{
			mysql_free_result(result);
		}
		result = NULL;
		mysql_close(handle);	
	} /*< for */

	return rval;
}


/**
 * Allocates a new hashtable and inserts database names and where to find them 
 * into it.
 * @param backends Backends to query for database names
 * @return Pointer to the newly allocated hashtable or NULL if an error occurred
 */
void* dbnames_hash_init(BACKEND** backends)
{
	HASHTABLE* htbl = hashtable_alloc(512,hashkeyfun,hashcmpfun);
	
	if(htbl == NULL)
	{
		LOGIF(LE, (skygw_log_write_flush(
				LOGFILE_ERROR,
				"Error: hashtable allocation failed.")));		
		return NULL;
	}
	/**Update the new hashtable with the key-value pairs*/
	if(!update_dbnames_hash(backends,htbl))
	{
		/*
		LOGIF(LE, (skygw_log_write_flush(LOGFILE_ERROR,"Errors encountered while querying databases.")));
		hashtable_free(htbl);
		return NULL;
		*/
		hashtable_free(htbl);
		htbl = NULL;
	}
	return htbl;
}

bool add_shard_info(GWBUF* buffer, char* target)
{
	HINT* hint = hint_create_route(NULL,HINT_ROUTE_TO_NAMED_SERVER,target);
	return (bool)gwbuf_add_hint(buffer,hint);
}

char* get_shard_target_name(ROUTER_INSTANCE* router, ROUTER_CLIENT_SES* client, GWBUF* buffer){
	HASHTABLE* ht = router->dbnames_hash;
	int sz = 0,i,j;
	char** dbnms = NULL;
	char* rval = NULL;

	bool has_dbs = false; /**If the query targets any database other than the current one*/

	if(!query_is_parsed(buffer)){
		parse_query(buffer);
	}

	dbnms = skygw_get_database_names(buffer,&sz);

	if(sz > 0){
		has_dbs = true;
		for(i = 0; i < sz; i++){

			if((rval = (char*)hashtable_fetch(ht,dbnms[i]))){				
				for(j = i;j < sz;j++) free(dbnms[j]);
				break;
			}
			free(dbnms[i]);
		}
		free(dbnms);
	}

	/**
	 * If the query contains no explicitly stated databases proceed to
	 * check if the session has an active database and if it is sharded.
	 */

	if(rval == NULL && !has_dbs && client->rses_mysql_session->db[0] != '\0'){
		rval = (char*)hashtable_fetch(ht,client->rses_mysql_session->db);
	}

	return rval;
}

/**
 * Implementation of the mandatory version entry point
 *
 * @return version string of the module
 */
char *
version()
{
	return version_str;
}

/**
 * The module initialisation routine, called when the module
 * is first loaded.
 */
void
ModuleInit()
{
        LOGIF(LM, (skygw_log_write_flush(
                           LOGFILE_MESSAGE,
                           "Initializing statemend-based read/write split router module.")));
        spinlock_init(&instlock);
        instances = NULL;
}

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
ROUTER_OBJECT* GetModuleObject()
{
        return &MyObject;
}

static void refreshInstance(
        ROUTER_INSTANCE*  router,
        CONFIG_PARAMETER* singleparam)
{
        CONFIG_PARAMETER*   param;
        bool                refresh_single;
	config_param_type_t paramtype;
	
        if (singleparam != NULL)
        {
                param = singleparam;
                refresh_single = true;
        }
        else
        {
                param = router->service->svc_config_param;
                refresh_single = false;
        }
        paramtype = config_get_paramtype(param);
	
        while (param != NULL)         
        {
		/** Catch unused parameter types */
		ss_dassert(paramtype == COUNT_TYPE || 
			paramtype == PERCENT_TYPE ||
			paramtype == SQLVAR_TARGET_TYPE);
		
                if (paramtype == COUNT_TYPE)
                {
                }
                else if (paramtype == PERCENT_TYPE)
                {
                }
		else if (paramtype == SQLVAR_TARGET_TYPE)
		{
			if (strncmp(param->name, 
				"use_sql_variables_in", 
				MAX_PARAM_LEN) == 0)
			{
				target_t valtarget;
				bool succp;
				
				succp = config_get_valtarget(&valtarget, param, NULL, paramtype);
				
				if (succp)
				{
					router->dbshard_config.rw_use_sql_variables_in = valtarget;
				}
			}
		}
		
                if (refresh_single)
                {
                        break;
                }
                param = param->next;
        }
        
#if defined(NOT_USED) /*< can't read monitor config parameters */
        if ((*router->servers)->backend_server->rlag == -2)
        {
                rlag_enabled = false;
        }
        else
        {
                rlag_enabled = true;
        }
        /** 
         * If replication lag detection is not enabled the measure can't be
         * used in slave selection.
         */
        if (!rlag_enabled)
        {
                if (rlag_limited)
                {
                        LOGIF(LE, (skygw_log_write_flush(
                                LOGFILE_ERROR,
                                "Warning : Configuration Failed, max_slave_replication_lag "
                                "is set to %d,\n\t\t      but detect_replication_lag "
                                "is not enabled. Replication lag will not be checked.",
                                router->dbshard_config.rw_max_slave_replication_lag)));
                }
            
                if (router->dbshard_config.rw_slave_select_criteria == 
                        LEAST_BEHIND_MASTER)
                {
                        LOGIF(LE, (skygw_log_write_flush(
                                LOGFILE_ERROR,
                                "Warning : Configuration Failed, router option "
                                "\n\t\t      slave_selection_criteria=LEAST_BEHIND_MASTER "
                                "is specified, but detect_replication_lag "
                                "is not enabled.\n\t\t      "
                                "slave_selection_criteria=%s will be used instead.",
                                STRCRITERIA(DEFAULT_CRITERIA))));
                        
                        router->dbshard_config.rw_slave_select_criteria =
                                DEFAULT_CRITERIA;
                }
        }
#endif /*< NOT_USED */

}

/**
 * Create an instance of dbshard statement router within the MaxScale.
 *
 * 
 * @param service	The service this router is being create for
 * @param options	The options for this query router
 *
 * @return NULL in failure, pointer to router in success.
 */
static ROUTER *
createInstance(SERVICE *service, char **options)
{
        ROUTER_INSTANCE*    router;
        SERVER*             server;
		//CONFIG_PARAMETER*  conf;
		//char*			    confval = NULL;
        int                 nservers;
        int                 i;
        
        if ((router = calloc(1, sizeof(ROUTER_INSTANCE))) == NULL) {
                return NULL; 
        } 
        router->service = service;
        spinlock_init(&router->lock);
        
        /** Calculate number of servers */
        server = service->databases;
        nservers = 0;
        
        while (server != NULL)
        {
                nservers++;
                server=server->nextdb;
        }
        router->servers = (BACKEND **)calloc(nservers + 1, sizeof(BACKEND *));
        
        if (router->servers == NULL)
        {
                free(router);
                return NULL;
        }
        /**
         * Create an array of the backend servers in the router structure to
         * maintain a count of the number of connections to each
         * backend server.
         */
        server = service->databases;
        nservers= 0;
        
        while (server != NULL) {
                if ((router->servers[nservers] = malloc(sizeof(BACKEND))) == NULL)
                {
			goto clean_up;
                }
                router->servers[nservers]->backend_server = server;
                router->servers[nservers]->backend_conn_count = 0;
                router->servers[nservers]->weight = 1;
                router->servers[nservers]->be_valid = false;
		if(server->monuser == NULL && service->credentials.name != NULL)
		{
			router->servers[nservers]->backend_server->monuser = 
				strdup(service->credentials.name);
		}
		if(server->monpw == NULL && service->credentials.authdata != NULL)
		{
			router->servers[nservers]->backend_server->monpw = 
				strdup(service->credentials.authdata);
		}
#if defined(SS_DEBUG)
                router->servers[nservers]->be_chk_top = CHK_NUM_BACKEND;
                router->servers[nservers]->be_chk_tail = CHK_NUM_BACKEND;
#endif
                nservers += 1;
                server = server->nextdb;
        }
        router->servers[nservers] = NULL;
   
	/**
	 * Process the options
	 */
	router->bitmask = 0;
	router->bitvalue = 0;


	/* Ignored for now
	conf = config_get_param(service->svc_config_param,"ignore_databases");
	if(conf)
	{
		confval = conf->value;
	}
	
    if(!parse_db_ignore_list(router,confval))
    {
		LOGIF(LE, (skygw_log_write_flush(
			LOGFILE_ERROR,
			"Error : Parsing of database ignore list failed. ")));
		goto clean_up;
	}
	*/

	/**
	 * Read config version number from service to inform what configuration 
	 * is used if any.
	 */
        router->dbshard_version = service->svc_config_version;

	/** refreshInstance(router, NULL); */
	/**
	 * Get hashtable which includes dbname,backend pairs
	 */
	router->dbnames_hash = (HASHTABLE*)dbnames_hash_init(router->servers);
	
	if (router->dbnames_hash == NULL)
	{
		LOGIF(LE, (skygw_log_write_flush(
			LOGFILE_ERROR,
			"Error : reading database names encountered an error. "
			"Router instance can't be created.")));
		goto clean_up;
	}
        /**
         * We have completed the creation of the router data, so now
         * insert this router into the linked list of routers
         * that have been created with this module.
         */
        spinlock_acquire(&instlock);
        router->next = instances;
        instances = router;
        spinlock_release(&instlock);
	goto retblock;
	
clean_up:
	/** clean up */
	for (i = 0; i < nservers; i++) 
	{
		free(router->servers[i]);
	}
	free(router->servers);
	free(router);
	router = NULL;
	/** Fallthrough */
retblock:
	return (ROUTER *)router;
}

/**
 * Associate a new session with this instance of the router.
 *
 * The session is used to store all the data required for a particular
 * client connection.
 *
 * @param instance	The router instance data
 * @param session	The session itself
 * @return Session specific data for this session
 */
static void* newSession(
        ROUTER*  router_inst,
        SESSION* session)
{
        backend_ref_t*      backend_ref; /*< array of backend references (DCB,BACKEND,cursor) */
        ROUTER_CLIENT_SES*  client_rses = NULL;
        ROUTER_INSTANCE*    router      = (ROUTER_INSTANCE *)router_inst;
        bool                succp;
        int                 router_nservers = 0; /*< # of servers in total */
        int                 i;
#if 0
	/** 
	 * It could be possibe to accept new session if some of the servers are 
	 * not reachable 
	 */
        const int           min_nservers = 1; /*< hard-coded for now */
#endif   
        client_rses = (ROUTER_CLIENT_SES *)calloc(1, sizeof(ROUTER_CLIENT_SES));
        
        if (client_rses == NULL)
        {
                ss_dassert(false);
                goto return_rses;
        }
#if defined(SS_DEBUG)
        client_rses->rses_chk_top = CHK_NUM_ROUTER_SES;
        client_rses->rses_chk_tail = CHK_NUM_ROUTER_SES;
#endif

	client_rses->router = router;
	client_rses->rses_mysql_session = (MYSQL_session*)session->data;
        /** 
         * If service config has been changed, reload config from service to 
         * router instance first.
         */
        spinlock_acquire(&router->lock);
        
	/**
	 * ??? tarvitaanko - ei vielä
	 */
#if 0
        if (router->service->svc_config_version > router->dbshard_version)
        {
                /** re-read all parameters to rwsplit config structure */
                refreshInstance(router, NULL); /*< scan through all parameters */
                /** increment rwsplit router's config version number */
                router->dbshard_version = router->service->svc_config_version;  
                /** Read options */
                dbshard_process_router_options(router, router->service->routerOptions);
        }
        /** Copy config struct from router instance */
        client_rses->rses_config = router->dbshard_config;
#endif   
        spinlock_release(&router->lock);
        /** 
         * Set defaults to session variables. 
	 * ??? tarvitaanko
         */
        client_rses->rses_autocommit_enabled = true;
        client_rses->rses_transaction_active = false;
        
	/**
	 * Instead of calling this, ensure that there is at least one 
	 * responding server.
	 */

        router_nservers = router_get_servercount(router);
#if 0
        if (!have_enough_servers(&client_rses, 
                                min_nservers, 
                                router_nservers, 
                                router))
        {
                goto return_rses;
        }
#endif
        /**
         * Create backend reference objects for this session.
         */
        backend_ref = (backend_ref_t *)calloc(1, router_nservers*sizeof(backend_ref_t));
        
        if (backend_ref == NULL)
        {
                /** log this */                        
                free(client_rses);
                free(backend_ref);
                client_rses = NULL;
                goto return_rses;
        }        
        /** 
         * Initialize backend references with BACKEND ptr.
         * Initialize session command cursors for each backend reference.
         */
        for (i=0; i< router_nservers; i++)
        {
#if defined(SS_DEBUG)
                backend_ref[i].bref_chk_top = CHK_NUM_BACKEND_REF;
                backend_ref[i].bref_chk_tail = CHK_NUM_BACKEND_REF;
                backend_ref[i].bref_sescmd_cur.scmd_cur_chk_top  = CHK_NUM_SESCMD_CUR;
                backend_ref[i].bref_sescmd_cur.scmd_cur_chk_tail = CHK_NUM_SESCMD_CUR;
#endif
                backend_ref[i].bref_state = 0;
                backend_ref[i].bref_backend = router->servers[i];
                /** store pointers to sescmd list to both cursors */
                backend_ref[i].bref_sescmd_cur.scmd_cur_rses = client_rses;
                backend_ref[i].bref_sescmd_cur.scmd_cur_active = false;
                backend_ref[i].bref_sescmd_cur.scmd_cur_ptr_property =
                        &client_rses->rses_properties[RSES_PROP_TYPE_SESCMD];
                backend_ref[i].bref_sescmd_cur.scmd_cur_cmd = NULL;   
        }
        
        spinlock_init(&client_rses->rses_lock);
        client_rses->rses_backend_ref = backend_ref;
        
        /**
         * Find a backend servers to connect to.
         * This command requires that rsession's lock is held.
         */
	if (!(succp = rses_begin_locked_router_action(client_rses)))
	{
                free(client_rses->rses_backend_ref);
                free(client_rses);
		client_rses = NULL;
                goto return_rses;
	}
	/**
	 * Connect to all backend servers
	 */
        succp = connect_backend_servers(backend_ref, 
					router_nservers,
					session,
					router);

        rses_end_locked_router_action(client_rses);
        
	/** 
	 * Master and at least <min_nslaves> slaves must be found 
	 */
        if (!succp) {
                free(client_rses->rses_backend_ref);
                free(client_rses);
                client_rses = NULL;
                goto return_rses;                
        }
        /** Copy backend pointers to router session. */
        client_rses->rses_capabilities = RCAP_TYPE_STMT_INPUT;
        client_rses->rses_backend_ref  = backend_ref;
        client_rses->rses_nbackends    = router_nservers; /*< # of backend servers */
        router->stats.n_sessions      += 1;
        
        /**
         * Version is bigger than zero once initialized.
         */
        atomic_add(&client_rses->rses_versno, 2);
        ss_dassert(client_rses->rses_versno == 2);
	/**
         * Add this session to end of the list of active sessions in router.
         */
	spinlock_acquire(&router->lock);
        client_rses->next   = router->connections;
        router->connections = client_rses;
        spinlock_release(&router->lock);

return_rses:    
#if defined(SS_DEBUG)
        if (client_rses != NULL)
        {
                CHK_CLIENT_RSES(client_rses);
        }
#endif
        return (void *)client_rses;
}



/**
 * Close a session with the router, this is the mechanism
 * by which a router may cleanup data structure etc.
 *
 * @param instance	The router instance data
 * @param session	The session being closed
 */
static void closeSession(
        ROUTER* instance,
        void*   router_session)
{
        ROUTER_CLIENT_SES* router_cli_ses;
        backend_ref_t*     backend_ref;

	LOGIF(LD, (skygw_log_write(LOGFILE_DEBUG,
			   "%lu [RWSplit:closeSession]",
			    pthread_self())));                                
	
        /** 
         * router session can be NULL if newSession failed and it is discarding
         * its connections and DCB's. 
         */
        if (router_session == NULL)
        {
                return;
        }
        router_cli_ses = (ROUTER_CLIENT_SES *)router_session;
        CHK_CLIENT_RSES(router_cli_ses);
        
        backend_ref = router_cli_ses->rses_backend_ref;
        /**
         * Lock router client session for secure read and update.
         */
        if (!router_cli_ses->rses_closed &&
                rses_begin_locked_router_action(router_cli_ses))
        {
		int i;
                /** 
                 * This sets router closed. Nobody is allowed to use router
                 * whithout checking this first.
                 */
                router_cli_ses->rses_closed = true;

                for (i=0; i<router_cli_ses->rses_nbackends; i++)
                {
                        backend_ref_t* bref = &backend_ref[i];
                        DCB* dcb = bref->bref_dcb;	
                        /** Close those which had been connected */
                        if (BREF_IS_IN_USE(bref))
                        {
                                CHK_DCB(dcb);
#if defined(SS_DEBUG)
				/**
				 * session must be moved to SESSION_STATE_STOPPING state before
				 * router session is closed.
				 */
				if (dcb->session != NULL)
				{
					ss_dassert(dcb->session->state == SESSION_STATE_STOPPING);
				}
#endif				
				/** Clean operation counter in bref and in SERVER */
                                while (BREF_IS_WAITING_RESULT(bref))
                                {
                                        bref_clear_state(bref, BREF_WAITING_RESULT);
                                }
                                bref_clear_state(bref, BREF_IN_USE);
                                bref_set_state(bref, BREF_CLOSED);
                                /**
                                 * closes protocol and dcb
                                 */
                                dcb_close(dcb);
                                /** decrease server current connection counters */
                                atomic_add(&bref->bref_backend->backend_server->stats.n_current, -1);
                                atomic_add(&bref->bref_backend->backend_conn_count, -1);
                        }
                }
                /** Unlock */
                rses_end_locked_router_action(router_cli_ses);                
        }
}

static void freeSession(
        ROUTER* router_instance,
        void*   router_client_session)
{
        ROUTER_CLIENT_SES* router_cli_ses;
        ROUTER_INSTANCE*   router;
	int                i;
        backend_ref_t*     backend_ref;
        
        router_cli_ses = (ROUTER_CLIENT_SES *)router_client_session;
        router         = (ROUTER_INSTANCE *)router_instance;
        backend_ref    = router_cli_ses->rses_backend_ref;
        
        for (i=0; i<router_cli_ses->rses_nbackends; i++)
        {
                if (!BREF_IS_IN_USE((&backend_ref[i])))
                {
                        continue;
                }
        }
        spinlock_acquire(&router->lock);

        if (router->connections == router_cli_ses) {
                router->connections = router_cli_ses->next;
        } else {
                ROUTER_CLIENT_SES* ptr = router->connections;

                while (ptr && ptr->next != router_cli_ses) {
                        ptr = ptr->next;
                }
            
                if (ptr) {
                        ptr->next = router_cli_ses->next;
                }
        }
        spinlock_release(&router->lock);
        
	/** 
	 * For each property type, walk through the list, finalize properties 
	 * and free the allocated memory. 
	 */
	for (i=RSES_PROP_TYPE_FIRST; i<RSES_PROP_TYPE_COUNT; i++)
	{
		rses_property_t* p = router_cli_ses->rses_properties[i];
		rses_property_t* q = p;
		
		while (p != NULL)
		{
			q = p->rses_prop_next;
			rses_property_done(p);
			p = q;
		}
	}
        /*
         * We are no longer in the linked list, free
         * all the memory and other resources associated
         * to the client session.
         */
        free(router_cli_ses->rses_backend_ref);
	free(router_cli_ses);
        return;
}

/**
 * Provide the router with a pointer to a suitable backend dcb. 
 * 
 * Detect failures in server statuses and reselect backends if necessary.
 * If name is specified, server name becomes primary selection criteria. 
 * Similarly, if max replication lag is specified, skip backends which lag too 
 * much.
 * 
 * @param p_dcb Address of the pointer to the resulting DCB
 * @param rses  Pointer to router client session
 * @param btype Backend type
 * @param name  Name of the backend which is primarily searched. May be NULL.
 * 
 * @return True if proper DCB was found, false otherwise.
 */
static bool get_shard_dcb(
        DCB**              p_dcb,
        ROUTER_CLIENT_SES* rses,
        char*              name)
{
        backend_ref_t* backend_ref;
        int            i;
        bool           succp = false;
        
        CHK_CLIENT_RSES(rses);
        ss_dassert(p_dcb != NULL && *(p_dcb) == NULL);
        
        if (p_dcb == NULL || name == NULL)
        {
                goto return_succp;
        }
        backend_ref = rses->rses_backend_ref;
	
	for (i=0; i<rses->rses_nbackends; i++)
	{
		BACKEND* b = backend_ref[i].bref_backend;			
		/**
		 * To become chosen:
		 * backend must be in use, name must match, and
		 * the backend state must be RUNNING
		 */
		if (BREF_IS_IN_USE((&backend_ref[i])) &&
			(strncasecmp(
				name,
				b->backend_server->unique_name, 
				PATH_MAX) == 0) &&
			SERVER_IS_RUNNING(b->backend_server))
		{
			*p_dcb = backend_ref[i].bref_dcb;
			succp = true; 
			ss_dassert(backend_ref[i].bref_dcb->state != DCB_STATE_ZOMBIE);
			goto return_succp;
		}
	}
        
return_succp:
        return succp;
}

/**
 * ??? Tarvitaanko tätä
 */
#if 0
/**
 * Find out which of the two backend servers has smaller value for select 
 * criteria property.
 * 
 * @param cand	previously selected candidate
 * @param new	challenger
 * @param sc	select criteria
 * 
 * @return pointer to backend reference of that backend server which has smaller
 * value in selection criteria. If either reference pointer is NULL then the 
 * other reference pointer value is returned.
 */
static backend_ref_t* check_candidate_bref(
	backend_ref_t* cand,
	backend_ref_t* new,
	select_criteria_t sc)
{
	int (*p)(const void *, const void *);
	/** get compare function */
	p = criteria_cmpfun[sc];
	
	if (new == NULL)
	{
		return cand;
	}
	else if (cand == NULL || (p((void *)cand,(void *)new) > 0))
	{
		return new;
	}
	else
	{
		return cand;
	}
}
#endif

/**
 * Examine the query type, transaction state and routing hints. Find out the
 * target for query routing.
 * 
 *  @param qtype      Type of query 
 *  @param trx_active Is transacation active or not
 *  @param hint       Pointer to list of hints attached to the query buffer
 * 
 *  @return bitfield including the routing target, or the target server name 
 *          if the query would otherwise be routed to slave.
 */
static route_target_t get_shard_route_target (
        skygw_query_type_t qtype,
        bool               trx_active, /*< !!! turha ? */
	target_t           use_sql_variables_in, /*< 'master' == single tässä tapauksessa */
        HINT*              hint) /*< !!! turha ? */
{
        route_target_t target = TARGET_UNDEFINED;

	/**
	 * These queries are not affected by hints
	 */
	if (QUERY_IS_TYPE(qtype, QUERY_TYPE_SESSION_WRITE) ||
		QUERY_IS_TYPE(qtype, QUERY_TYPE_PREPARE_STMT) ||
		QUERY_IS_TYPE(qtype, QUERY_TYPE_PREPARE_NAMED_STMT) ||
		/** Configured to allow writing variables to all nodes */
		(use_sql_variables_in == TYPE_ALL &&
			QUERY_IS_TYPE(qtype, QUERY_TYPE_GSYSVAR_WRITE)) ||
		/** enable or disable autocommit are always routed to all */
		QUERY_IS_TYPE(qtype, QUERY_TYPE_ENABLE_AUTOCOMMIT) ||
		QUERY_IS_TYPE(qtype, QUERY_TYPE_DISABLE_AUTOCOMMIT))
	{
		/** hints don't affect on routing */
		target = TARGET_ALL;
	}
	else
	{
		target = TARGET_NAMED_SERVER;
	}
#if defined(SS_DEBUG)
	LOGIF(LT, (skygw_log_write(
		LOGFILE_TRACE,
		"Selected target \"%s\"",
		STRTARGET(target))));
#endif
	return target;
}

/**
 * Check if the query is a DROP TABLE... query and
 * if it targets a temporary table, remove it from the hashtable.
 * @param instance Router instance
 * @param router_session Router client session
 * @param querybuf GWBUF containing the query
 * @param type The type of the query resolved so far
 */
void check_drop_tmp_table(
        ROUTER* instance,
        void*   router_session,
        GWBUF*  querybuf,
	skygw_query_type_t type)
{

  int tsize = 0, klen = 0,i;
  char** tbl = NULL;
  char *hkey,*dbname;

  ROUTER_CLIENT_SES* router_cli_ses = (ROUTER_CLIENT_SES *)router_session;
  rses_property_t*   rses_prop_tmp;

  rses_prop_tmp = router_cli_ses->rses_properties[RSES_PROP_TYPE_TMPTABLES];
  dbname = router_cli_ses->rses_mysql_session->db;

  if (is_drop_table_query(querybuf))
    {
      tbl = skygw_get_table_names(querybuf,&tsize,false);
	  if(tbl != NULL){		
		  for(i = 0; i<tsize; i++)
			  {
				  klen = strlen(dbname) + strlen(tbl[i]) + 2;
				  hkey = calloc(klen,sizeof(char));
				  strcpy(hkey,dbname);
				  strcat(hkey,".");
				  strcat(hkey,tbl[i]);
			
				  if (rses_prop_tmp && 
					  rses_prop_tmp->rses_prop_data.temp_tables)
					  {
						  if (hashtable_delete(rses_prop_tmp->rses_prop_data.temp_tables, 
											   (void *)hkey))
							  {
								  LOGIF(LT, (skygw_log_write(LOGFILE_TRACE,
															 "Temporary table dropped: %s",hkey)));
							  }
					  }
				  free(tbl[i]);
				  free(hkey);
			  }

		  free(tbl);
	  }
    }
}

/**
 * Check if the query targets a temporary table.
 * @param instance Router instance
 * @param router_session Router client session
 * @param querybuf GWBUF containing the query
 * @param type The type of the query resolved so far
 * @return The type of the query
 */
skygw_query_type_t is_read_tmp_table(
				     ROUTER* instance,
				     void*   router_session,
				     GWBUF*  querybuf,
	skygw_query_type_t type)
{

  bool target_tmp_table = false;
  int tsize = 0, klen = 0,i;
  char** tbl = NULL;
  char *hkey,*dbname;

  ROUTER_CLIENT_SES* router_cli_ses = (ROUTER_CLIENT_SES *)router_session;
  skygw_query_type_t qtype = type;
  rses_property_t*   rses_prop_tmp;

  rses_prop_tmp = router_cli_ses->rses_properties[RSES_PROP_TYPE_TMPTABLES];
  dbname = router_cli_ses->rses_mysql_session->db;

  if (QUERY_IS_TYPE(qtype, QUERY_TYPE_READ) || 
	  QUERY_IS_TYPE(qtype, QUERY_TYPE_LOCAL_READ) ||
	  QUERY_IS_TYPE(qtype, QUERY_TYPE_USERVAR_READ) ||
	  QUERY_IS_TYPE(qtype, QUERY_TYPE_SYSVAR_READ) ||
	  QUERY_IS_TYPE(qtype, QUERY_TYPE_GSYSVAR_READ))	  
    {
      tbl = skygw_get_table_names(querybuf,&tsize,false);

      if (tbl != NULL && tsize > 0)
	{ 
	  /** Query targets at least one table */
	  for(i = 0; i<tsize && !target_tmp_table && tbl[i]; i++)
	    {
	      klen = strlen(dbname) + strlen(tbl[i]) + 2;
	      hkey = calloc(klen,sizeof(char));
	      strcpy(hkey,dbname);
	      strcat(hkey,".");
	      strcat(hkey,tbl[i]);

	      if (rses_prop_tmp && 
		  rses_prop_tmp->rses_prop_data.temp_tables)
		{
				
		  if( (target_tmp_table = 
		       (bool)hashtable_fetch(rses_prop_tmp->rses_prop_data.temp_tables,(void *)hkey)))
		    {
		      /**Query target is a temporary table*/
		      qtype = QUERY_TYPE_READ_TMP_TABLE;			
		      LOGIF(LT, 
			    (skygw_log_write(LOGFILE_TRACE,
					     "Query targets a temporary table: %s",hkey)));
		    }
		}

	      free(hkey);
	    }

	}
    }

	
	if(tbl != NULL){
		for(i = 0; i<tsize;i++)
			{
				free(tbl[i]);
			}
		free(tbl);
	}
	
	return qtype;
}

/** 
 * If query is of type QUERY_TYPE_CREATE_TMP_TABLE then find out 
 * the database and table name, create a hashvalue and 
 * add it to the router client session's property. If property 
 * doesn't exist then create it first.
 * @param instance Router instance
 * @param router_session Router client session
 * @param querybuf GWBUF containing the query
 * @param type The type of the query resolved so far
 */ 
void check_create_tmp_table(
			    ROUTER* instance,
			    void*   router_session,
			    GWBUF*  querybuf,
			    skygw_query_type_t type)
{

  int klen = 0;
  char *hkey,*dbname;

  ROUTER_CLIENT_SES* router_cli_ses = (ROUTER_CLIENT_SES *)router_session;
  rses_property_t*   rses_prop_tmp;
  HASHTABLE*	   h;

  rses_prop_tmp = router_cli_ses->rses_properties[RSES_PROP_TYPE_TMPTABLES];
  dbname = router_cli_ses->rses_mysql_session->db;


  if (QUERY_IS_TYPE(type, QUERY_TYPE_CREATE_TMP_TABLE))
    {
      bool  is_temp = true;
      char* tblname = NULL;
		
      tblname = skygw_get_created_table_name(querybuf);
		
      if (tblname && strlen(tblname) > 0)
	{
	  klen = strlen(dbname) + strlen(tblname) + 2;
	  hkey = calloc(klen,sizeof(char));
	  strcpy(hkey,dbname);
	  strcat(hkey,".");
	  strcat(hkey,tblname);
	}
      else
	{
	  hkey = NULL;
	}
		
      if(rses_prop_tmp == NULL)
	{
	  if((rses_prop_tmp = 
	      (rses_property_t*)calloc(1,sizeof(rses_property_t))))
	    {
#if defined(SS_DEBUG)
	      rses_prop_tmp->rses_prop_chk_top = CHK_NUM_ROUTER_PROPERTY;
	      rses_prop_tmp->rses_prop_chk_tail = CHK_NUM_ROUTER_PROPERTY;
#endif
	      rses_prop_tmp->rses_prop_rsession = router_cli_ses;
	      rses_prop_tmp->rses_prop_refcount = 1;
	      rses_prop_tmp->rses_prop_next = NULL;
	      rses_prop_tmp->rses_prop_type = RSES_PROP_TYPE_TMPTABLES;
	      router_cli_ses->rses_properties[RSES_PROP_TYPE_TMPTABLES] = rses_prop_tmp;
	    }
	  else
		{
		  LOGIF(LE, (skygw_log_write_flush(LOGFILE_ERROR,"Error : Call to malloc() failed.")));
		}
	}
	  if(rses_prop_tmp){
      if (rses_prop_tmp->rses_prop_data.temp_tables == NULL)
	{
	  h = hashtable_alloc(7, hashkeyfun, hashcmpfun);
	  hashtable_memory_fns(h,hstrdup,NULL,hfree,NULL);
	  if (h != NULL)
	    {
	      rses_prop_tmp->rses_prop_data.temp_tables = h;
	    }else{
		  LOGIF(LE, (skygw_log_write_flush(LOGFILE_ERROR,"Error : Failed to allocate a new hashtable.")));
	  }

	}
		
     if (hkey && rses_prop_tmp->rses_prop_data.temp_tables &&
	  hashtable_add(rses_prop_tmp->rses_prop_data.temp_tables,
			(void *)hkey,
			(void *)is_temp) == 0) /*< Conflict in hash table */
	{
	  LOGIF(LT, (skygw_log_write(
				     LOGFILE_TRACE,
				     "Temporary table conflict in hashtable: %s",
				     hkey)));
	}
#if defined(SS_DEBUG)
      {
	bool retkey = 
	  hashtable_fetch(
			  rses_prop_tmp->rses_prop_data.temp_tables,
			  hkey);
	if (retkey)
	  {
	    LOGIF(LT, (skygw_log_write(
				       LOGFILE_TRACE,
				       "Temporary table added: %s",
				       hkey)));
	  }
      }
#endif
	  }
	  
      free(hkey);
      free(tblname);
    }
}

/**
 * The main routing entry, this is called with every packet that is
 * received and has to be forwarded to the backend database.
 *
 * The routeQuery will make the routing decision based on the contents
 * of the instance, session and the query itself in the queue. The
 * data in the queue may not represent a complete query, it represents
 * the data that has been received. The query router itself is responsible
 * for buffering the partial query, a later call to the query router will
 * contain the remainder, or part thereof of the query.
 *
 * @param instance		The query router instance
 * @param router_session	The session associated with the client
 * @param querybuf		MaxScale buffer queue with received packet
 *
 * @return if succeed 1, otherwise 0
 * If routeQuery fails, it means that router session has failed.
 * In any tolerated failure, handleError is called and if necessary,
 * an error message is sent to the client.
 * 
 * For now, routeQuery don't tolerate errors, so any error will close
 * the session. vraa 14.6.14
 */
static int routeQuery(
        ROUTER* instance,
        void*   router_session,
        GWBUF*  querybuf)
{
        skygw_query_type_t qtype          = QUERY_TYPE_UNKNOWN;
        mysql_server_cmd_t packet_type;
        uint8_t*           packet;
        int                ret            = 0;
        DCB*               target_dcb     = NULL;
        ROUTER_INSTANCE*   inst = (ROUTER_INSTANCE *)instance;
        ROUTER_CLIENT_SES* router_cli_ses = (ROUTER_CLIENT_SES *)router_session;
        bool               rses_is_closed = false;
        route_target_t     route_target;
	bool           	   succp          = false;
	char* tname = NULL;

        CHK_CLIENT_RSES(router_cli_ses);

        /** Dirty read for quick check if router is closed. */
        if (router_cli_ses->rses_closed)
        {
                rses_is_closed = true;
        }
        ss_dassert(!GWBUF_IS_TYPE_UNDEFINED(querybuf));
        
        packet = GWBUF_DATA(querybuf);
        packet_type = packet[4];

	if (rses_is_closed)
        {
                /** 
                 * MYSQL_COM_QUIT may have sent by client and as a part of backend 
                 * closing procedure.
                 */
                if (packet_type != MYSQL_COM_QUIT)
                {
                        char* query_str = modutil_get_query(querybuf);
                        
                        LOGIF(LE, 
                                (skygw_log_write_flush(
                                LOGFILE_ERROR,
                                "Error: Can't route %s:%s:\"%s\" to "
                                "backend server. Router is closed.",
                                STRPACKETTYPE(packet_type),
                                STRQTYPE(qtype),
                                (query_str == NULL ? "(empty)" : query_str))));
			free(query_str);
                }
                ret = 0;
                goto retblock;
        }
        	
        /** If buffer is not contiguous, make it such */
	if (querybuf->next != NULL)
	{
		querybuf = gwbuf_make_contiguous(querybuf);
	}

        switch(packet_type) {
                case MYSQL_COM_QUIT:        /*< 1 QUIT will close all sessions */
                case MYSQL_COM_INIT_DB:     /*< 2 DDL must go to the master */
                case MYSQL_COM_REFRESH:     /*< 7 - I guess this is session but not sure */
                case MYSQL_COM_DEBUG:       /*< 0d all servers dump debug info to stdout */
                case MYSQL_COM_PING:        /*< 0e all servers are pinged */
                case MYSQL_COM_CHANGE_USER: /*< 11 all servers change it accordingly */
                case MYSQL_COM_STMT_CLOSE:  /*< free prepared statement */
                case MYSQL_COM_STMT_SEND_LONG_DATA: /*< send data to column */
                case MYSQL_COM_STMT_RESET:  /*< resets the data of a prepared statement */
                        qtype = QUERY_TYPE_SESSION_WRITE;
                        break;
                        
                case MYSQL_COM_CREATE_DB:   /**< 5 DDL must go to the master */
                case MYSQL_COM_DROP_DB:     /**< 6 DDL must go to the master */
                        qtype = QUERY_TYPE_WRITE;
                        break;

                case MYSQL_COM_QUERY:
                        qtype = query_classifier_get_type(querybuf);
                        break;
                        
                case MYSQL_COM_STMT_PREPARE:
                        qtype = query_classifier_get_type(querybuf);
                        qtype |= QUERY_TYPE_PREPARE_STMT;
                        break;
                        
                case MYSQL_COM_STMT_EXECUTE:
                        /** Parsing is not needed for this type of packet */
                        qtype = QUERY_TYPE_EXEC_STMT;
                        break;
                        
                case MYSQL_COM_SHUTDOWN:       /**< 8 where should shutdown be routed ? */
                case MYSQL_COM_STATISTICS:     /**< 9 ? */
                case MYSQL_COM_PROCESS_INFO:   /**< 0a ? */
                case MYSQL_COM_CONNECT:        /**< 0b ? */
                case MYSQL_COM_PROCESS_KILL:   /**< 0c ? */
                case MYSQL_COM_TIME:           /**< 0f should this be run in gateway ? */
                case MYSQL_COM_DELAYED_INSERT: /**< 10 ? */
                case MYSQL_COM_DAEMON:         /**< 1d ? */
                default:
                        break;
        } /**< switch by packet type */

        if (packet_type == MYSQL_COM_INIT_DB)
	{
		if (!change_current_db(inst, router_cli_ses, querybuf))
		{
			LOGIF(LE, (skygw_log_write_flush(
				LOGFILE_ERROR,
				"Error : Changing database failed.")));
		}
		ret = 1;
		goto retblock;
	}
        
        /**
	 * !!! Temporary tablen tutkiminen voi olla turhaa. Poista tarvittaessa.
	 */
	/**
	 * Check if the query has anything to do with temporary tables.
	 */
	qtype = is_read_tmp_table(instance,router_session,querybuf,qtype);
        check_create_tmp_table(instance,router_session,querybuf,qtype);
        check_drop_tmp_table(instance,router_session,querybuf,qtype);

	/**
	 * !!! Transaktion tutkiminen voi olla turhaa paitsi jos haluataan 
	 * lokittaa. Poista tarvittaessa.
	 */
        /**
          * If autocommit is disabled or transaction is explicitly started
          * transaction becomes active and master gets all statements until
          * transaction is committed and autocommit is enabled again.
          */
        if (router_cli_ses->rses_autocommit_enabled &&
                QUERY_IS_TYPE(qtype, QUERY_TYPE_DISABLE_AUTOCOMMIT))
        {
                router_cli_ses->rses_autocommit_enabled = false;
                
                if (!router_cli_ses->rses_transaction_active)
                {
                        router_cli_ses->rses_transaction_active = true;
                }
        }
        else if (!router_cli_ses->rses_transaction_active &&
                QUERY_IS_TYPE(qtype, QUERY_TYPE_BEGIN_TRX))
        {
                router_cli_ses->rses_transaction_active = true;
        }
        /** 
         * Explicit COMMIT and ROLLBACK, implicit COMMIT.
         */
        if (router_cli_ses->rses_autocommit_enabled &&
                router_cli_ses->rses_transaction_active &&
                (QUERY_IS_TYPE(qtype,QUERY_TYPE_COMMIT) ||
                QUERY_IS_TYPE(qtype,QUERY_TYPE_ROLLBACK)))
        {
                router_cli_ses->rses_transaction_active = false;
        } 
        else if (!router_cli_ses->rses_autocommit_enabled &&
                QUERY_IS_TYPE(qtype, QUERY_TYPE_ENABLE_AUTOCOMMIT))
        {
                router_cli_ses->rses_autocommit_enabled = true;
                router_cli_ses->rses_transaction_active = false;
	}        

	if (LOG_IS_ENABLED(LOGFILE_TRACE))
	{
		uint8_t*      packet = GWBUF_DATA(querybuf);
		unsigned char ptype = packet[4];
		size_t        len = MIN(GWBUF_LENGTH(querybuf), 
					 MYSQL_GET_PACKET_LEN((unsigned char *)querybuf->start)-1);
		char*         data = (char*)&packet[5];
		char*         contentstr = strndup(data, len);
		char*         qtypestr = skygw_get_qtype_str(qtype);

		skygw_log_write(
			LOGFILE_TRACE,
			"> Autocommit: %s, trx is %s, cmd: %s, type: %s, "
			"stmt: %s%s %s",
			(router_cli_ses->rses_autocommit_enabled ? "[enabled]" : "[disabled]"),
			(router_cli_ses->rses_transaction_active ? "[open]" : "[not open]"),
			STRPACKETTYPE(ptype),
			(qtypestr==NULL ? "N/A" : qtypestr),
			contentstr,
			(querybuf->hint == NULL ? "" : ", Hint:"),
			(querybuf->hint == NULL ? "" : STRHINTTYPE(querybuf->hint->type)));

		free(contentstr);
		free(qtypestr);
	}
	/**
	 * Find out whether the query should be routed to single server or to 
	 * all of them.
	 */
        /** 
         * Find out where to route the query. Result may not be clear; it is 
         * possible to have a hint for routing to a named server which can
         * be either slave or master. 
         * If query would otherwise be routed to slave then the hint determines 
         * actual target server if it exists.
         * 
         * route_target is a bitfield and may include :
	 * TARGET_ALL
	 * - route to all connected backend servers
	 * TARGET_SLAVE[|TARGET_NAMED_SERVER|TARGET_RLAG_MAX]
	 * - route primarily according to hints, then to slave and if those
	 *   failed, eventually to master
	 * TARGET_MASTER[|TARGET_NAMED_SERVER|TARGET_RLAG_MAX]
	 * - route primarily according to the hints and if they failed, 
	 *   eventually to master
         */

	/**
	 * Update the hashtable
	 */

	if(inst->update_hash)
    {
		update_dbnames_hash(inst->servers,inst->dbnames_hash);
	}

	if((tname = get_shard_target_name(inst,router_cli_ses,querybuf)) != NULL)
	{
			route_target = TARGET_NAMED_SERVER;
	}
	else
	{

		route_target = get_shard_route_target(qtype, 
				    router_cli_ses->rses_transaction_active,
					router_cli_ses->rses_config.rw_use_sql_variables_in,
					querybuf->hint);
	}

	if (TARGET_IS_ALL(route_target))
	{
		/**
		 * It is not sure if the session command in question requires
		 * response. Statement is examined in route_session_write.
		 * Router locking is done inside the function.
		 */
		succp = route_session_write(router_cli_ses, 
					    gwbuf_clone(querybuf), 
					    inst, 
					    packet_type, 
					    qtype);
		
		if (succp)
		{
			atomic_add(&inst->stats.n_all, 1);
			ret = 1;
		}
		goto retblock;
	}
	
	/** Lock router session */
	if (!rses_begin_locked_router_action(router_cli_ses))
	{
		LOGIF(LT, (skygw_log_write(
			LOGFILE_TRACE,
			"Route query aborted! Routing session is closed <")));
		ret = 0;
		goto retblock;
	}
	/**
	 * Query is routed to one of the backends
	 */
	if (TARGET_IS_NAMED_SERVER(route_target))
	{	
		/**
		 * Search backend server by name or replication lag. 
		 * If it fails, then try to find valid slave or master.
		 */ 

		succp = get_shard_dcb(&target_dcb, router_cli_ses, tname);

		if (!succp)
		{
			LOGIF(LT, (skygw_log_write(
				LOGFILE_TRACE,
				"Was supposed to route to named server "
				"%s but couldn't find the server in a "
				"suitable state.",
				tname)));
		}
	}

	if (succp) /*< Have DCB of the target backend */
	{
		backend_ref_t*   bref;
		sescmd_cursor_t* scur;
		
		bref = get_bref_from_dcb(router_cli_ses, target_dcb);
		scur = &bref->bref_sescmd_cur;

		LOGIF(LT, (skygw_log_write(
			LOGFILE_TRACE,
			"Route query to \t%s:%d <",
			bref->bref_backend->backend_server->name,
			bref->bref_backend->backend_server->port)));
		/** 
		 * Store current stmt if execution of previous session command 
		 * haven't completed yet. Note that according to MySQL protocol
		 * there can only be one such non-sescmd stmt at the time.
		 */
		if (sescmd_cursor_is_active(scur))
		{
			ss_dassert(bref->bref_pending_cmd == NULL);
			bref->bref_pending_cmd = gwbuf_clone(querybuf);
			
			rses_end_locked_router_action(router_cli_ses);
			ret = 1;
			goto retblock;
		}
			
		if ((ret = target_dcb->func.write(target_dcb, gwbuf_clone(querybuf))) == 1)
		{
			backend_ref_t* bref;
			
			atomic_add(&inst->stats.n_queries, 1);
			/**
			 * Add one query response waiter to backend reference
			 */
			bref = get_bref_from_dcb(router_cli_ses, target_dcb);
			bref_set_state(bref, BREF_QUERY_ACTIVE);
			bref_set_state(bref, BREF_WAITING_RESULT);
		}
		else
		{
			LOGIF(LE, (skygw_log_write_flush(
				LOGFILE_ERROR,
				"Error : Routing query failed.")));
		}
	}
	rses_end_locked_router_action(router_cli_ses);
retblock:
#if defined(SS_DEBUG2)
        {
                char* canonical_query_str;
                
                canonical_query_str = skygw_get_canonical(querybuf);
                
                if (canonical_query_str != NULL)
                {
                        LOGIF(LT, (skygw_log_write(
                                LOGFILE_TRACE,
                                "Canonical version: %s",
                                canonical_query_str)));
                        free(canonical_query_str);
                }
        }
#endif
        gwbuf_free(querybuf);
        return ret;
}



/** 
 * @node Acquires lock to router client session if it is not closed.
 *
 * Parameters:
 * @param rses - in, use
 *          
 *
 * @return true if router session was not closed. If return value is true
 * it means that router is locked, and must be unlocked later. False, if
 * router was closed before lock was acquired.
 *
 * 
 * @details (write detailed description here)
 *
 */
static bool rses_begin_locked_router_action(
        ROUTER_CLIENT_SES* rses)
{
        bool succp = false;
        
        CHK_CLIENT_RSES(rses);

        if (rses->rses_closed) {
                
                goto return_succp;
        }       
        spinlock_acquire(&rses->rses_lock);
        if (rses->rses_closed) {
                spinlock_release(&rses->rses_lock);
                goto return_succp;
        }       
        succp = true;
        
return_succp:
        return succp;
}

/** to be inline'd */
/** 
 * @node Releases router client session lock.
 *
 * Parameters:
 * @param rses - <usage>
 *          <description>
 *
 * @return void
 *
 * 
 * @details (write detailed description here)
 *
 */
static void rses_end_locked_router_action(
        ROUTER_CLIENT_SES* rses)
{
        CHK_CLIENT_RSES(rses);
        spinlock_release(&rses->rses_lock);
}


/**
 * Diagnostics routine
 *
 * Print query router statistics to the DCB passed in
 *
 * @param	instance	The router instance
 * @param	dcb		The DCB for diagnostic output
 */
static	void
diagnostic(ROUTER *instance, DCB *dcb)
{
ROUTER_CLIENT_SES *router_cli_ses;
ROUTER_INSTANCE	  *router = (ROUTER_INSTANCE *)instance;
int		  i = 0;
BACKEND		  *backend;
char		  *weightby;

	spinlock_acquire(&router->lock);
	router_cli_ses = router->connections;
	while (router_cli_ses)
	{
		i++;
		router_cli_ses = router_cli_ses->next;
	}
	spinlock_release(&router->lock);
	
	dcb_printf(dcb,
                   "\tNumber of router sessions:           	%d\n",
                   router->stats.n_sessions);
	dcb_printf(dcb,
                   "\tCurrent no. of router sessions:      	%d\n",
                   i);
	dcb_printf(dcb,
                   "\tNumber of queries forwarded:          	%d\n",
                   router->stats.n_queries);
	dcb_printf(dcb,
                   "\tNumber of queries forwarded to master:	%d\n",
                   router->stats.n_master);
	dcb_printf(dcb,
                   "\tNumber of queries forwarded to slave: 	%d\n",
                   router->stats.n_slave);
	dcb_printf(dcb,
                   "\tNumber of queries forwarded to all:   	%d\n",
                   router->stats.n_all);
	if ((weightby = serviceGetWeightingParameter(router->service)) != NULL)
        {
                dcb_printf(dcb,
		   "\tConnection distribution based on %s "
                                "server parameter.\n", weightby);
                dcb_printf(dcb,
                        "\t\tServer               Target %%    Connections  "
			"Operations\n");
                dcb_printf(dcb,
                        "\t\t                               Global  Router\n");
                for (i = 0; router->servers[i]; i++)
                {
                        backend = router->servers[i];
                        dcb_printf(dcb,
				"\t\t%-20s %3.1f%%     %-6d  %-6d  %d\n",
                                backend->backend_server->unique_name,
                                (float)backend->weight / 10,
				backend->backend_server->stats.n_current,
				backend->backend_conn_count,
				backend->backend_server->stats.n_current_ops);
                }

        }

}

/**
 * Client Reply routine
 *
 * The routine will reply to client for session change with master server data
 *
 * @param	instance	The router instance
 * @param	router_session	The router session 
 * @param	backend_dcb	The backend DCB
 * @param	queue		The GWBUF with reply data
 */
static void clientReply (
        ROUTER* instance,
        void*   router_session,
        GWBUF*  writebuf,
        DCB*    backend_dcb)
{
        DCB*               client_dcb;
        ROUTER_CLIENT_SES* router_cli_ses;
	sescmd_cursor_t*   scur = NULL;
        backend_ref_t*     bref;
        
	router_cli_ses = (ROUTER_CLIENT_SES *)router_session;
        CHK_CLIENT_RSES(router_cli_ses);

        /**
         * Lock router client session for secure read of router session members.
         * Note that this could be done without lock by using version #
         */
        if (!rses_begin_locked_router_action(router_cli_ses))
        {
                print_error_packet(router_cli_ses, writebuf, backend_dcb);
                goto lock_failed;
	}
        /** Holding lock ensures that router session remains open */
        ss_dassert(backend_dcb->session != NULL);
	client_dcb = backend_dcb->session->client;

        /** Unlock */
        rses_end_locked_router_action(router_cli_ses);        
	/**
         * 1. Check if backend received reply to sescmd.
         * 2. Check sescmd's state whether OK_PACKET has been
         *    sent to client already and if not, lock property cursor,
         *    reply to client, and move property cursor forward. Finally
         *    release the lock.
         * 3. If reply for this sescmd is sent, lock property cursor
         *    and 
         */
	if (client_dcb == NULL)
	{
                while ((writebuf = gwbuf_consume(
                        writebuf, 
                        GWBUF_LENGTH(writebuf))) != NULL);
		/** Log that client was closed before reply */
                goto lock_failed;
	}
	/** Lock router session */
        if (!rses_begin_locked_router_action(router_cli_ses))
        {
                /** Log to debug that router was closed */
                goto lock_failed;
        }
        bref = get_bref_from_dcb(router_cli_ses, backend_dcb);

#if !defined(FOR_BUG548_FIX_ONLY)
	/** This makes the issue becoming visible in poll.c */
	if (bref == NULL)
	{
		/** Unlock router session */
		rses_end_locked_router_action(router_cli_ses);
		goto lock_failed;
	}
#endif
	
        CHK_BACKEND_REF(bref);
        scur = &bref->bref_sescmd_cur;
        /**
         * Active cursor means that reply is from session command 
         * execution.
         */
	if (sescmd_cursor_is_active(scur))
	{
                if (LOG_IS_ENABLED(LOGFILE_ERROR) && 
                        MYSQL_IS_ERROR_PACKET(((uint8_t *)GWBUF_DATA(writebuf))))
                {
                        uint8_t* buf = 
                                (uint8_t *)GWBUF_DATA((scur->scmd_cur_cmd->my_sescmd_buf));
			uint8_t* replybuf = (uint8_t *)GWBUF_DATA(writebuf);
			size_t   len      = MYSQL_GET_PACKET_LEN(buf);
			size_t   replylen = MYSQL_GET_PACKET_LEN(replybuf);
			char*    cmdstr   = strndup(&((char *)buf)[5], len-4);
			char*    err      = strndup(&((char *)replybuf)[8], 5);
			char*    replystr = strndup(&((char *)replybuf)[13], 
						    replylen-4-5);
			
                        ss_dassert(len+4 == GWBUF_LENGTH(scur->scmd_cur_cmd->my_sescmd_buf));
                        
                        LOGIF(LE, (skygw_log_write_flush(
                                LOGFILE_ERROR,
                                "Error : Failed to execute %s in %s:%d. %s %s",
                                cmdstr, 
                                bref->bref_backend->backend_server->name,
                                bref->bref_backend->backend_server->port,
				err,
				replystr)));
                        
                        free(cmdstr);
			free(err);
			free(replystr);
                }
                
                if (GWBUF_IS_TYPE_SESCMD_RESPONSE(writebuf))
                {
                        /** 
                        * Discard all those responses that have already been sent to
                        * the client. Return with buffer including response that
                        * needs to be sent to client or NULL.
                        */
                        writebuf = sescmd_cursor_process_replies(writebuf, bref);
                }
                /** 
                 * If response will be sent to client, decrease waiter count.
                 * This applies to session commands only. Counter decrement
                 * for other type of queries is done outside this block.
                 */
                if (writebuf != NULL && client_dcb != NULL)
                {
                        /** Set response status as replied */
                        bref_clear_state(bref, BREF_WAITING_RESULT);
                }
	}
	/**
         * Clear BREF_QUERY_ACTIVE flag and decrease waiter counter.
         * This applies for queries  other than session commands.
         */
	else if (BREF_IS_QUERY_ACTIVE(bref))
	{
                bref_clear_state(bref, BREF_QUERY_ACTIVE);
                /** Set response status as replied */
                bref_clear_state(bref, BREF_WAITING_RESULT);
        }

        if (writebuf != NULL && client_dcb != NULL)
        {
                /** Write reply to client DCB */
		SESSION_ROUTE_REPLY(backend_dcb->session, writebuf);
        }
        /** Unlock router session */
        rses_end_locked_router_action(router_cli_ses);
        
        /** Lock router session */
        if (!rses_begin_locked_router_action(router_cli_ses))
        {
                /** Log to debug that router was closed */
                goto lock_failed;
        }
        /** There is one pending session command to be executed. */
        if (sescmd_cursor_is_active(scur)) 
        {
                bool succp;
                
                LOGIF(LT, (skygw_log_write(
                        LOGFILE_TRACE,
                        "Backend %s:%d processed reply and starts to execute "
                        "active cursor.",
                        bref->bref_backend->backend_server->name,
                        bref->bref_backend->backend_server->port)));
                
                succp = execute_sescmd_in_backend(bref);
                
                ss_dassert(succp);
        }
	else if (bref->bref_pending_cmd != NULL) /*< non-sescmd is waiting to be routed */
	{
		int ret;
		
		CHK_GWBUF(bref->bref_pending_cmd);
		
		if ((ret = bref->bref_dcb->func.write(bref->bref_dcb, 
			gwbuf_clone(bref->bref_pending_cmd))) == 1)
		{
			ROUTER_INSTANCE* inst = (ROUTER_INSTANCE *)instance;
			atomic_add(&inst->stats.n_queries, 1);
			/**
			 * Add one query response waiter to backend reference
			 */
			bref_set_state(bref, BREF_QUERY_ACTIVE);
			bref_set_state(bref, BREF_WAITING_RESULT);
		}
		else
		{
			LOGIF(LE, (skygw_log_write_flush(
				LOGFILE_ERROR,
				"Error : Routing query \"%s\" failed.",
				bref->bref_pending_cmd)));
		}
		gwbuf_free(bref->bref_pending_cmd);
		bref->bref_pending_cmd = NULL;
	}
	/** Unlock router session */
        rses_end_locked_router_action(router_cli_ses);
        
lock_failed:
        return;
}

/** Compare nunmber of connections from this router in backend servers */
int bref_cmp_router_conn(
        const void* bref1,
        const void* bref2)
{
        BACKEND* b1 = ((backend_ref_t *)bref1)->bref_backend;
        BACKEND* b2 = ((backend_ref_t *)bref2)->bref_backend;

        return ((1000 * b1->backend_conn_count) / b1->weight)
			  - ((1000 * b2->backend_conn_count) / b2->weight);
}

/** Compare nunmber of global connections in backend servers */
int bref_cmp_global_conn(
        const void* bref1,
        const void* bref2)
{
        BACKEND* b1 = ((backend_ref_t *)bref1)->bref_backend;
        BACKEND* b2 = ((backend_ref_t *)bref2)->bref_backend;
        
        return ((1000 * b1->backend_server->stats.n_current) / b1->weight)
		  - ((1000 * b2->backend_server->stats.n_current) / b2->weight);
}


/** Compare relication lag between backend servers */
int bref_cmp_behind_master(
        const void* bref1, 
        const void* bref2)
{
        BACKEND* b1 = ((backend_ref_t *)bref1)->bref_backend;
        BACKEND* b2 = ((backend_ref_t *)bref2)->bref_backend;
        
        return ((b1->backend_server->rlag < b2->backend_server->rlag) ? -1 :
        ((b1->backend_server->rlag > b2->backend_server->rlag) ? 1 : 0));
}

/** Compare nunmber of current operations in backend servers */
int bref_cmp_current_load(
        const void* bref1,
        const void* bref2)
{
        SERVER*  s1 = ((backend_ref_t *)bref1)->bref_backend->backend_server;
        SERVER*  s2 = ((backend_ref_t *)bref2)->bref_backend->backend_server;
        BACKEND* b1 = ((backend_ref_t *)bref1)->bref_backend;
        BACKEND* b2 = ((backend_ref_t *)bref2)->bref_backend;
        
        return ((1000 * s1->stats.n_current_ops) - b1->weight)
			- ((1000 * s2->stats.n_current_ops) - b2->weight);
}
        
static void bref_clear_state(
        backend_ref_t* bref,
        bref_state_t   state)
{
        if (state != BREF_WAITING_RESULT)
        {
                bref->bref_state &= ~state;
        }
        else
        {
                int prev1;
                int prev2;
                
                /** Decrease waiter count */
                prev1 = atomic_add(&bref->bref_num_result_wait, -1);
                
                if (prev1 <= 0) {
                        atomic_add(&bref->bref_num_result_wait, 1);
                }
                else
                {
                        /** Decrease global operation count */
                        prev2 = atomic_add(
                                &bref->bref_backend->backend_server->stats.n_current_ops, -1);
                        ss_dassert(prev2 > 0);
                }       
        }
}

static void bref_set_state(        
        backend_ref_t* bref,
        bref_state_t   state)
{
        if (state != BREF_WAITING_RESULT)
        {
                bref->bref_state |= state;
        }
        else
        {
                int prev1;
                int prev2;
                
                /** Increase waiter count */
                prev1 = atomic_add(&bref->bref_num_result_wait, 1);
                ss_dassert(prev1 >= 0);
                
                /** Increase global operation count */
                prev2 = atomic_add(
                        &bref->bref_backend->backend_server->stats.n_current_ops, 1);
                ss_dassert(prev2 >= 0);                
        }
}

/** 
 * @node Search all RUNNING backend servers and connect
 *
 * Parameters:
 * @param backend_ref - in, use, out 
 *      Pointer to backend server reference object array.
 *      NULL is not allowed.
 *
 * @param router_nservers - in, use
 *      Number of backend server pointers pointed to by b.
 * 
 * @param session - in, use
 *      MaxScale session pointer used when connection to backend is established.
 *
 * @param  router - in, use
 *      Pointer to router instance. Used when server states are qualified.
 * 
 * @return true, if at least one master and one slave was found.
 *
 * 
 * @details It is assumed that there is only one available server.
 *      There will be exactly as many backend references than there are 
 * 	connections because all servers are supposed to be operational. It is,
 * 	however, possible that there are less available servers than expected.
 */
static bool connect_backend_servers(
        backend_ref_t*     backend_ref,
        int                router_nservers,
        SESSION*           session,
        ROUTER_INSTANCE*   router)
{
        bool            succp = true;
	/*
	bool		is_synced_master;
	bool		master_connected = true;
	*/
        int             servers_found = 0;
        int             servers_connected = 0;
        int             slaves_connected = 0;
        int             i;
	/*
	select_criteria_t select_criteria = LEAST_GLOBAL_CONNECTIONS;
	*/
#if 0
		        if (router->bitvalue != 0) /*< 'synced' is the only bitvalue in rwsplit */
        {
                is_synced_master = true;
        }
        else
        {
                is_synced_master = false;
        }
#endif

#if defined(EXTRA_SS_DEBUG)        
        LOGIF(LT, (skygw_log_write(LOGFILE_TRACE, "Servers and conns before ordering:")));
        
        for (i=0; i<router_nservers; i++)
        {
                BACKEND* b = backend_ref[i].bref_backend;

                LOGIF(LT, (skygw_log_write(LOGFILE_TRACE, 
                                           "bref %p %d %s %d:%d",
                                           &backend_ref[i],
                                           backend_ref[i].bref_state,
                                           b->backend_server->name,
                                           b->backend_server->port,
                                           b->backend_conn_count)));                
        }
#endif

        if (LOG_IS_ENABLED(LOGFILE_TRACE))
        {
		LOGIF(LT, (skygw_log_write(LOGFILE_TRACE, 
			"Servers and connection counts:")));

		for (i=0; i<router_nservers; i++)
		{
			BACKEND* b = backend_ref[i].bref_backend;
			
			LOGIF(LT, (skygw_log_write_flush(LOGFILE_TRACE, 
				"MaxScale connections : %d (%d) in \t%s:%d %s",
				b->backend_conn_count,
				b->backend_server->stats.n_current,
				b->backend_server->name,
				b->backend_server->port,
				STRSRVSTATUS(b->backend_server))));
		}
	} /*< log only */        
        /**
         * Scan server list and connect each of them. None should fail or session
	 * can't be established.
         */
        for (i=0; i < router_nservers; i++)
        {
                BACKEND* b = backend_ref[i].bref_backend;

                if (SERVER_IS_RUNNING(b->backend_server) &&
                        ((b->backend_server->status & router->bitmask) ==
                        router->bitvalue))
                {
			servers_found += 1;
                                
			/** Server is already connected */
			if (BREF_IS_IN_USE((&backend_ref[i])))
			{
				slaves_connected += 1;
			}
			/** New server connection */
			else
			{
				backend_ref[i].bref_dcb = dcb_connect(
					b->backend_server,
					session,
					b->backend_server->protocol);
				
				if (backend_ref[i].bref_dcb != NULL)
				{
					servers_connected += 1;
					/**
					 * Start executing session command
					 * history.
					 */
					execute_sescmd_history(&backend_ref[i]);
					
					/**
					 * When server fails, this callback
					 * is called.
					 * !!! Todo, routine which removes 
					 * corresponding entries from the hash 
					 * table.
					 */
#if 0
					dcb_add_callback(
						backend_ref[i].bref_dcb,
						DCB_REASON_NOT_RESPONDING,
						&router_handle_state_switch,
						(void *)&backend_ref[i]);
#endif
					backend_ref[i].bref_state = 0;
					bref_set_state(&backend_ref[i], 
						       BREF_IN_USE);
					/** 
					 * Increase backend connection counter.
					 * Server's stats are _increased_ in 
					 * dcb.c:dcb_alloc !
					 * But decreased in the calling function 
					 * of dcb_close.
					 */
					atomic_add(&b->backend_conn_count, 1);
				}
				else
				{
					succp = false;
					LOGIF(LE, (skygw_log_write_flush(
						LOGFILE_ERROR,
						"Error : Unable to establish "
						"connection with slave %s:%d",
						b->backend_server->name,
						b->backend_server->port)));
					/* handle connect error */
					break;
				}
			}
		}
        } /*< for */
        
#if defined(EXTRA_SS_DEBUG)        
        LOGIF(LT, (skygw_log_write(LOGFILE_TRACE, "Servers and conns after ordering:")));
        
        for (i=0; i<router_nservers; i++)
        {
                BACKEND* b = backend_ref[i].bref_backend;
                
		LOGIF(LT, (skygw_log_write(LOGFILE_TRACE, 
					"bref %p %d %s %d:%d",
					&backend_ref[i],
					backend_ref[i].bref_state,
					b->backend_server->name,
					b->backend_server->port,
					b->backend_conn_count)));
        }
#endif        
        /**
         * Successful cases
         */
        if (servers_connected == router_nservers)
        {
		succp = true;
		
		if (LOG_IS_ENABLED(LT))
		{
			for (i=0; i<router_nservers; i++)
			{
				BACKEND* b = backend_ref[i].bref_backend;
				
				if (BREF_IS_IN_USE((&backend_ref[i])))
				{                                        
					LOGIF(LT, (skygw_log_write(
						LOGFILE_TRACE,
						"Connected %s in \t%s:%d",
						STRSRVSTATUS(b->backend_server),
						b->backend_server->name,
						b->backend_server->port)));
				}
			} /* for */
		}
	}
	else
	{
		LOGIF(LE, (skygw_log_write(
			LOGFILE_ERROR,
			"Warning : Couldn't connect to all available "
			"servers. Session can't be created.")));
		
		/** Clean up connections */
		for (i=0; i<router_nservers; i++)
		{
			if (BREF_IS_IN_USE((&backend_ref[i])))
			{
				ss_dassert(backend_ref[i].bref_backend->backend_conn_count > 0);
				
				/** disconnect opened connections */
				dcb_close(backend_ref[i].bref_dcb);
				bref_clear_state(&backend_ref[i], BREF_IN_USE);
				/** Decrease backend's connection counter. */
				atomic_add(&backend_ref[i].bref_backend->backend_conn_count, -1);
			}
		}
	}
        return succp;
}


/** 
 * Create a generic router session property strcture.
 */
static rses_property_t* rses_property_init(
	rses_property_type_t prop_type)
{
	rses_property_t* prop;
	
	prop = (rses_property_t*)calloc(1, sizeof(rses_property_t));
	if (prop == NULL)
	{
		goto return_prop;
	}
	prop->rses_prop_type = prop_type;
#if defined(SS_DEBUG)
	prop->rses_prop_chk_top = CHK_NUM_ROUTER_PROPERTY;
	prop->rses_prop_chk_tail = CHK_NUM_ROUTER_PROPERTY;
#endif
	
return_prop:
	CHK_RSES_PROP(prop);
	return prop;
}

/**
 * Property is freed at the end of router client session.
 */
static void rses_property_done(
	rses_property_t* prop)
{
	CHK_RSES_PROP(prop);
	
	switch (prop->rses_prop_type) {
	case RSES_PROP_TYPE_SESCMD:
		mysql_sescmd_done(&prop->rses_prop_data.sescmd);
		break;
		
	case RSES_PROP_TYPE_TMPTABLES:
		hashtable_free(prop->rses_prop_data.temp_tables);
		break;
		
	default:
		LOGIF(LD, (skygw_log_write(
                                   LOGFILE_DEBUG,
                                   "%lu [rses_property_done] Unknown property type %d "
                                   "in property %p",
                                   pthread_self(),
                                   prop->rses_prop_type,
                                   prop)));
		
		ss_dassert(false);
		break;
	}
	free(prop);
}

/**
 * Add property to the router_client_ses structure's rses_properties
 * array. The slot is determined by the type of property.
 * In each slot there is a list of properties of similar type.
 * 
 * Router client session must be locked.
 */
static void rses_property_add(
        ROUTER_CLIENT_SES* rses,
        rses_property_t*   prop)
{
        rses_property_t* p;
        
        CHK_CLIENT_RSES(rses);
        CHK_RSES_PROP(prop);
        ss_dassert(SPINLOCK_IS_LOCKED(&rses->rses_lock));
        
        prop->rses_prop_rsession = rses;
        p = rses->rses_properties[prop->rses_prop_type];
        
        if (p == NULL)
        {
                rses->rses_properties[prop->rses_prop_type] = prop;
        }
        else
        {
                while (p->rses_prop_next != NULL)
                {
                        p = p->rses_prop_next;
                }
                p->rses_prop_next = prop;
        }
}

/** 
 * Router session must be locked.
 * Return session command pointer if succeed, NULL if failed.
 */
static mysql_sescmd_t* rses_property_get_sescmd(
        rses_property_t* prop)
{
        mysql_sescmd_t* sescmd;
        
        CHK_RSES_PROP(prop);
        ss_dassert(prop->rses_prop_rsession == NULL ||
                SPINLOCK_IS_LOCKED(&prop->rses_prop_rsession->rses_lock));
        
        sescmd = &prop->rses_prop_data.sescmd;
        
        if (sescmd != NULL)
        {
                CHK_MYSQL_SESCMD(sescmd);
        }
        return sescmd;
}
       
/**
static void rses_begin_locked_property_action(
        rses_property_t* prop)
{
        CHK_RSES_PROP(prop);
        spinlock_acquire(&prop->rses_prop_lock);
}

static void rses_end_locked_property_action(
        rses_property_t* prop)
{
        CHK_RSES_PROP(prop);
        spinlock_release(&prop->rses_prop_lock);
}
*/

/**
 * Create session command property.
 */
static mysql_sescmd_t* mysql_sescmd_init (
        rses_property_t*   rses_prop,
        GWBUF*             sescmd_buf,
        unsigned char      packet_type,
        ROUTER_CLIENT_SES* rses)
{
        mysql_sescmd_t* sescmd;
        
        CHK_RSES_PROP(rses_prop);
        /** Can't call rses_property_get_sescmd with uninitialized sescmd */
        sescmd = &rses_prop->rses_prop_data.sescmd;
        sescmd->my_sescmd_prop = rses_prop; /*< reference to owning property */
#if defined(SS_DEBUG)
        sescmd->my_sescmd_chk_top  = CHK_NUM_MY_SESCMD;
        sescmd->my_sescmd_chk_tail = CHK_NUM_MY_SESCMD;
#endif
        /** Set session command buffer */
        sescmd->my_sescmd_buf  = sescmd_buf;
        sescmd->my_sescmd_packet_type = packet_type;
        
        return sescmd;
}


static void mysql_sescmd_done(
	mysql_sescmd_t* sescmd)
{
	CHK_RSES_PROP(sescmd->my_sescmd_prop);
	gwbuf_free(sescmd->my_sescmd_buf);
        memset(sescmd, 0, sizeof(mysql_sescmd_t));
}

/**
 * All cases where backend message starts at least with one response to session
 * command are handled here.
 * Read session commands from property list. If command is already replied,
 * discard packet. Else send reply to client. In both cases move cursor forward
 * until all session command replies are handled. 
 * 
 * Cases that are expected to happen and which are handled:
 * s = response not yet replied to client, S = already replied response,
 * q = query
 * 1. q+        for example : select * from mysql.user
 * 2. s+        for example : set autocommit=1
 * 3. S+        
 * 4. sq+
 * 5. Sq+
 * 6. Ss+
 * 7. Ss+q+
 * 8. S+q+
 * 9. s+q+
 */
static GWBUF* sescmd_cursor_process_replies(
        GWBUF*           replybuf,
        backend_ref_t*   bref)
{
        mysql_sescmd_t*  scmd;
        sescmd_cursor_t* scur;
        
        scur = &bref->bref_sescmd_cur;        
        ss_dassert(SPINLOCK_IS_LOCKED(&(scur->scmd_cur_rses->rses_lock)));
        scmd = sescmd_cursor_get_command(scur);
               
        CHK_GWBUF(replybuf);
        
        /** 
         * Walk through packets in the message and the list of session 
         * commands. 
         */
        while (scmd != NULL && replybuf != NULL)
        {
                /** Faster backend has already responded to client : discard */
                if (scmd->my_sescmd_is_replied)
                {
                        bool last_packet = false;
                        
                        CHK_GWBUF(replybuf);
                        
                        while (!last_packet)
                        {
                                int  buflen;
                                
                                buflen = GWBUF_LENGTH(replybuf);
                                last_packet = GWBUF_IS_TYPE_RESPONSE_END(replybuf);
                                /** discard packet */
                                replybuf = gwbuf_consume(replybuf, buflen);
                        }
                        /** Set response status received */
                        bref_clear_state(bref, BREF_WAITING_RESULT);
                }
                /** Response is in the buffer and it will be sent to client. */
                else if (replybuf != NULL)
                {
                        /** Mark the rest session commands as replied */
                        scmd->my_sescmd_is_replied = true;
                }
                
                if (sescmd_cursor_next(scur))
                {
                        scmd = sescmd_cursor_get_command(scur);
                }
                else
                {
                        scmd = NULL;
                        /** All session commands are replied */
                        scur->scmd_cur_active = false;
                }
        }
        ss_dassert(replybuf == NULL || *scur->scmd_cur_ptr_property == NULL);
        
        return replybuf;
}



/**
 * Get the address of current session command.
 * 
 * Router session must be locked */
static mysql_sescmd_t* sescmd_cursor_get_command(
	sescmd_cursor_t* scur)
{
        mysql_sescmd_t* scmd;
        
        ss_dassert(SPINLOCK_IS_LOCKED(&(scur->scmd_cur_rses->rses_lock)));
        scur->scmd_cur_cmd = rses_property_get_sescmd(*scur->scmd_cur_ptr_property);
        
        CHK_MYSQL_SESCMD(scur->scmd_cur_cmd);
        
        scmd = scur->scmd_cur_cmd;
      
	return scmd;
}

/** router must be locked */
static bool sescmd_cursor_is_active(
	sescmd_cursor_t* sescmd_cursor)
{
	bool succp;
        ss_dassert(SPINLOCK_IS_LOCKED(&sescmd_cursor->scmd_cur_rses->rses_lock));

        succp = sescmd_cursor->scmd_cur_active;
	return succp;
}

/** router must be locked */
static void sescmd_cursor_set_active(
        sescmd_cursor_t* sescmd_cursor,
        bool             value)
{
        ss_dassert(SPINLOCK_IS_LOCKED(&sescmd_cursor->scmd_cur_rses->rses_lock));
        /** avoid calling unnecessarily */
        ss_dassert(sescmd_cursor->scmd_cur_active != value);
        sescmd_cursor->scmd_cur_active = value;
}

/** 
 * Clone session command's command buffer. 
 * Router session must be locked 
 */
static GWBUF* sescmd_cursor_clone_querybuf(
	sescmd_cursor_t* scur)
{
	GWBUF* buf;
	ss_dassert(scur->scmd_cur_cmd != NULL);
	
	buf = gwbuf_clone(scur->scmd_cur_cmd->my_sescmd_buf);
	
	CHK_GWBUF(buf);
	return buf;
}

static bool sescmd_cursor_history_empty(
        sescmd_cursor_t* scur)
{
        bool succp;
        
        CHK_SESCMD_CUR(scur);
        
        if (scur->scmd_cur_rses->rses_properties[RSES_PROP_TYPE_SESCMD] == NULL)
        {
                succp = true;
        }
        else
        {
                succp = false;
        }
        
        return succp;
}


static void sescmd_cursor_reset(
        sescmd_cursor_t* scur)
{
        ROUTER_CLIENT_SES* rses;
        CHK_SESCMD_CUR(scur);
        CHK_CLIENT_RSES(scur->scmd_cur_rses);
        rses = scur->scmd_cur_rses;

        scur->scmd_cur_ptr_property = &rses->rses_properties[RSES_PROP_TYPE_SESCMD];
        
        CHK_RSES_PROP((*scur->scmd_cur_ptr_property));
        scur->scmd_cur_active = false;
        scur->scmd_cur_cmd = &(*scur->scmd_cur_ptr_property)->rses_prop_data.sescmd;
}

static bool execute_sescmd_history(
        backend_ref_t* bref)
{
        bool             succp;
        sescmd_cursor_t* scur;
        CHK_BACKEND_REF(bref);
        
        scur = &bref->bref_sescmd_cur;
        CHK_SESCMD_CUR(scur);
 
        if (!sescmd_cursor_history_empty(scur))
        {
                sescmd_cursor_reset(scur);
                succp = execute_sescmd_in_backend(bref);
        }
        else
        {
                succp = true;
        }

        return succp;
}

/**
 * If session command cursor is passive, sends the command to backend for
 * execution. 
 *  
 * Returns true if command was sent or added successfully to the queue.
 * Returns false if command sending failed or if there are no pending session
 * 	commands.
 * 
 * Router session must be locked.
 */ 
static bool execute_sescmd_in_backend(
        backend_ref_t* backend_ref)
{
	DCB*             dcb;
	bool             succp;
	int              rc = 0;
	sescmd_cursor_t* scur;

        if (BREF_IS_CLOSED(backend_ref))
        {
                succp = false;
                goto return_succp;
        }
        dcb = backend_ref->bref_dcb;
        
	CHK_DCB(dcb);
 	CHK_BACKEND_REF(backend_ref);
	
        /** 
         * Get cursor pointer and copy of command buffer to cursor.
         */
        scur = &backend_ref->bref_sescmd_cur;

        /** Return if there are no pending ses commands */
	if (sescmd_cursor_get_command(scur) == NULL)
	{
		succp = false;
                LOGIF(LT, (skygw_log_write_flush(
                        LOGFILE_TRACE,
                        "Cursor had no pending session commands.")));
                
                goto return_succp;
	}

	if (!sescmd_cursor_is_active(scur))
        {
                /** Cursor is left active when function returns. */
                sescmd_cursor_set_active(scur, true);
        }
#if defined(SS_DEBUG)
        LOGIF(LT, tracelog_routed_query(scur->scmd_cur_rses, 
                                        "execute_sescmd_in_backend", 
                                        backend_ref, 
                                        sescmd_cursor_clone_querybuf(scur)));

        {
                GWBUF* tmpbuf = sescmd_cursor_clone_querybuf(scur);
                uint8_t* ptr = GWBUF_DATA(tmpbuf);
                unsigned char cmd = MYSQL_GET_COMMAND(ptr);
                
                LOGIF(LD, (skygw_log_write(
                        LOGFILE_DEBUG,
                        "%lu [execute_sescmd_in_backend] Just before write, fd "
                        "%d : cmd %s.",
                        pthread_self(),
                        dcb->fd,
                        STRPACKETTYPE(cmd))));
                gwbuf_free(tmpbuf);
        }
#endif /*< SS_DEBUG */
        switch (scur->scmd_cur_cmd->my_sescmd_packet_type) {
                case MYSQL_COM_CHANGE_USER:
			/** This makes it possible to handle replies correctly */
			gwbuf_set_type(scur->scmd_cur_cmd->my_sescmd_buf, GWBUF_TYPE_SESCMD);
			rc = dcb->func.auth(
                                dcb, 
                                NULL, 
                                dcb->session, 
                                sescmd_cursor_clone_querybuf(scur));
                        break;

		case MYSQL_COM_INIT_DB:
		{
			/**
			 * Record database name and store to session.
			 */
			GWBUF* tmpbuf;
			MYSQL_session* data;
			unsigned int qlen;

			data = dcb->session->data;
			tmpbuf = scur->scmd_cur_cmd->my_sescmd_buf;
			qlen = MYSQL_GET_PACKET_LEN((unsigned char*)tmpbuf->start);
			memset(data->db,0,MYSQL_DATABASE_MAXLEN+1);
			strncpy(data->db,tmpbuf->start+5,qlen - 1);			
		}
		/** Fallthrough */
		case MYSQL_COM_QUERY:
                default:
                        /** 
                         * Mark session command buffer, it triggers writing 
                         * MySQL command to protocol
                         */
                        gwbuf_set_type(scur->scmd_cur_cmd->my_sescmd_buf, GWBUF_TYPE_SESCMD);
                        rc = dcb->func.write(
                                dcb, 
                                sescmd_cursor_clone_querybuf(scur));
                        break;
        }

        if (rc == 1)
        {
                succp = true;
        }
        else
        {
                succp = false;
        }
return_succp:
	return succp;
}


/**
 * Moves cursor to next property and copied address of its sescmd to cursor.
 * Current propery must be non-null.
 * If current property is the last on the list, *scur->scmd_ptr_property == NULL
 * 
 * Router session must be locked 
 */
static bool sescmd_cursor_next(
	sescmd_cursor_t* scur)
{
	bool             succp = false;
	rses_property_t* prop_curr;
	rses_property_t* prop_next;

        ss_dassert(scur != NULL);
        ss_dassert(*(scur->scmd_cur_ptr_property) != NULL);
        ss_dassert(SPINLOCK_IS_LOCKED(
                &(*(scur->scmd_cur_ptr_property))->rses_prop_rsession->rses_lock));

        /** Illegal situation */
	if (scur == NULL ||
           *scur->scmd_cur_ptr_property == NULL ||
            scur->scmd_cur_cmd == NULL)
	{
		/** Log error */
		goto return_succp;
	}
	prop_curr = *(scur->scmd_cur_ptr_property);

        CHK_MYSQL_SESCMD(scur->scmd_cur_cmd);
        ss_dassert(prop_curr == mysql_sescmd_get_property(scur->scmd_cur_cmd));
        CHK_RSES_PROP(prop_curr);

        /** Copy address of pointer to next property */
        scur->scmd_cur_ptr_property = &(prop_curr->rses_prop_next);
        prop_next = *scur->scmd_cur_ptr_property;
        ss_dassert(prop_next == *(scur->scmd_cur_ptr_property));
        
        
	/** If there is a next property move forward */
	if (prop_next != NULL)
	{
                CHK_RSES_PROP(prop_next);
                CHK_RSES_PROP((*(scur->scmd_cur_ptr_property)));

                /** Get pointer to next property's sescmd */
                scur->scmd_cur_cmd = rses_property_get_sescmd(prop_next);

                ss_dassert(prop_next == scur->scmd_cur_cmd->my_sescmd_prop);                
                CHK_MYSQL_SESCMD(scur->scmd_cur_cmd);
                CHK_RSES_PROP(scur->scmd_cur_cmd->my_sescmd_prop);
	}
	else
	{
		/** No more properties, can't proceed. */
		goto return_succp;
	}

	if (scur->scmd_cur_cmd != NULL)
	{
                succp = true;
        }
        else
        {
                ss_dassert(false); /*< Log error, sescmd shouldn't be NULL */
        }
return_succp:
	return succp;
}

static rses_property_t* mysql_sescmd_get_property(
	mysql_sescmd_t* scmd)
{
	CHK_MYSQL_SESCMD(scmd);
	return scmd->my_sescmd_prop;
}

static void tracelog_routed_query(
        ROUTER_CLIENT_SES* rses,
        char*              funcname,
        backend_ref_t*     bref,
        GWBUF*             buf)
{
        uint8_t*       packet = GWBUF_DATA(buf);
        unsigned char  packet_type = packet[4];
        size_t         len;
        size_t         buflen = GWBUF_LENGTH(buf);
        char*          querystr;
        char*          startpos = (char *)&packet[5];
        BACKEND*       b;
        backend_type_t be_type;
        DCB*           dcb;
        
        CHK_BACKEND_REF(bref);
        b = bref->bref_backend;
        CHK_BACKEND(b);
        dcb = bref->bref_dcb;
        CHK_DCB(dcb);
        
        be_type = BACKEND_TYPE(b);

        if (GWBUF_IS_TYPE_MYSQL(buf))
        {
                len  = packet[0];
                len += 256*packet[1];
                len += 256*256*packet[2];
                
                if (packet_type == '\x03') 
                {
                        querystr = (char *)malloc(len);
                        memcpy(querystr, startpos, len-1);
                        querystr[len-1] = '\0';
                        LOGIF(LD, (skygw_log_write_flush(
                                LOGFILE_DEBUG,
                                "%lu [%s] %d bytes long buf, \"%s\" -> %s:%d %s dcb %p",
                                pthread_self(),
                                funcname,
                                buflen,
                                querystr,
                                b->backend_server->name,
                                b->backend_server->port, 
                                STRBETYPE(be_type),
                                dcb)));
                        free(querystr);
                }
                else if (packet_type == '\x22' || 
                        packet_type == 0x22 || 
                        packet_type == '\x26' || 
                        packet_type == 0x26 ||
                        true)
                {
                        querystr = (char *)malloc(len);
                        memcpy(querystr, startpos, len-1);
                        querystr[len-1] = '\0';
                        LOGIF(LD, (skygw_log_write_flush(
                                LOGFILE_DEBUG,
                                "%lu [%s] %d bytes long buf, \"%s\" -> %s:%d %s dcb %p",
                                pthread_self(),
                                funcname,
                                buflen,
                                querystr,
                                b->backend_server->name,
                                b->backend_server->port, 
                                STRBETYPE(be_type),
                                dcb)));
                        free(querystr);                        
                }
        }
        gwbuf_free(buf);
}


/**
 * Return rc, rc < 0 if router session is closed. rc == 0 if there are no 
 * capabilities specified, rc > 0 when there are capabilities.
 */
static uint8_t getCapabilities (
        ROUTER* inst,
        void*   router_session)
{
        ROUTER_CLIENT_SES* rses = (ROUTER_CLIENT_SES *)router_session;
        uint8_t            rc;
        
        if (!rses_begin_locked_router_action(rses))
        {
                rc = 0xff;
                goto return_rc;
        }
        rc = rses->rses_capabilities;
        
        rses_end_locked_router_action(rses);
        
return_rc:
        return rc;
}

/**
 * Execute in backends used by current router session.
 * Save session variable commands to router session property
 * struct. Thus, they can be replayed in backends which are 
 * started and joined later.
 * 
 * Suppress redundant OK packets sent by backends.
 * 
 * The first OK packet is replied to the client.
 * Return true if succeed, false is returned if router session was closed or
 * if execute_sescmd_in_backend failed.
 */
static bool route_session_write(
        ROUTER_CLIENT_SES* router_cli_ses,
        GWBUF*             querybuf,
        ROUTER_INSTANCE*   inst,
        unsigned char      packet_type,
        skygw_query_type_t qtype)
{
        bool              succp;
        rses_property_t*  prop;
        backend_ref_t*    backend_ref;
        int               i;
  
        LOGIF(LT, (skygw_log_write(
                LOGFILE_TRACE,
                "Session write, routing to all servers.")));

        backend_ref = router_cli_ses->rses_backend_ref;
        
        /**
         * These are one-way messages and server doesn't respond to them.
         * Therefore reply processing is unnecessary and session 
         * command property is not needed. It is just routed to all available
         * backends.
         */
        if (packet_type == MYSQL_COM_STMT_SEND_LONG_DATA ||
                packet_type == MYSQL_COM_QUIT ||
                packet_type == MYSQL_COM_STMT_CLOSE)
        {
                int rc;
               
                succp = true;
                
                /** Lock router session */
                if (!rses_begin_locked_router_action(router_cli_ses))
                {
                        succp = false;
                        goto return_succp;
                }
                                
                for (i=0; i<router_cli_ses->rses_nbackends; i++)
                {
                        DCB* dcb = backend_ref[i].bref_dcb;     
			
			if (LOG_IS_ENABLED(LOGFILE_TRACE))
			{
				LOGIF(LT, (skygw_log_write(
					LOGFILE_TRACE,
					"Route query to %s\t%s:%d%s",
					(SERVER_IS_MASTER(backend_ref[i].bref_backend->backend_server) ? 
						"master" : "slave"),
					backend_ref[i].bref_backend->backend_server->name,
					backend_ref[i].bref_backend->backend_server->port,
					(i+1==router_cli_ses->rses_nbackends ? " <" : ""))));
			}

                        if (BREF_IS_IN_USE((&backend_ref[i])))
                        {
                                rc = dcb->func.write(dcb, gwbuf_clone(querybuf));
                        
                                if (rc != 1)
                                {
                                        succp = false;
                                }
                        }
                }
                rses_end_locked_router_action(router_cli_ses);
                gwbuf_free(querybuf);
                goto return_succp;
        }
        /** Lock router session */
        if (!rses_begin_locked_router_action(router_cli_ses))
        {
                succp = false;
                goto return_succp;
        }
        
        if (router_cli_ses->rses_nbackends <= 0)
	{
		succp = false;
		goto return_succp;
	}
        /** 
         * Additional reference is created to querybuf to 
         * prevent it from being released before properties
         * are cleaned up as a part of router sessionclean-up.
         */
        prop = rses_property_init(RSES_PROP_TYPE_SESCMD);
        mysql_sescmd_init(prop, querybuf, packet_type, router_cli_ses);
        
        /** Add sescmd property to router client session */
        rses_property_add(router_cli_ses, prop);
         
        for (i=0; i<router_cli_ses->rses_nbackends; i++)
        {
                if (BREF_IS_IN_USE((&backend_ref[i])))
                {
                        sescmd_cursor_t* scur;
                        
			if (LOG_IS_ENABLED(LOGFILE_TRACE))
			{
				LOGIF(LT, (skygw_log_write(
					LOGFILE_TRACE,
					"Route query to %s\t%s:%d%s",
					(SERVER_IS_MASTER(backend_ref[i].bref_backend->backend_server) ? 
					"master" : "slave"),
					backend_ref[i].bref_backend->backend_server->name,
					backend_ref[i].bref_backend->backend_server->port,
					(i+1==router_cli_ses->rses_nbackends ? " <" : ""))));
			}
			
                        scur = backend_ref_get_sescmd_cursor(&backend_ref[i]);
                        
                        /** 
                         * Add one waiter to backend reference.
                         */
                        bref_set_state(get_bref_from_dcb(router_cli_ses, 
                                                         backend_ref[i].bref_dcb), 
							BREF_WAITING_RESULT);
                        /** 
                         * Start execution if cursor is not already executing.
                         * Otherwise, cursor will execute pending commands
                         * when it completes with previous commands.
                         */
                        if (sescmd_cursor_is_active(scur))
                        {
                                succp = true;
                                
                                LOGIF(LT, (skygw_log_write(
                                        LOGFILE_TRACE,
                                        "Backend %s:%d already executing sescmd.",
                                        backend_ref[i].bref_backend->backend_server->name,
                                        backend_ref[i].bref_backend->backend_server->port)));
                        }
                        else
                        {
                                succp = execute_sescmd_in_backend(&backend_ref[i]);
                                
                                if (!succp)
                                {
                                        LOGIF(LE, (skygw_log_write_flush(
                                                LOGFILE_ERROR,
                                                "Error : Failed to execute session "
                                                "command in %s:%d",
                                                backend_ref[i].bref_backend->backend_server->name,
                                                backend_ref[i].bref_backend->backend_server->port)));
                                }
                        }
                }
                else
		{
			succp = false;
		}
        }
        /** Unlock router session */
        rses_end_locked_router_action(router_cli_ses);
               
return_succp:
        return succp;
}

#if defined(NOT_USED)

static bool router_option_configured(
        ROUTER_INSTANCE* router,
        const char*      optionstr,
        void*            data)
{
        bool   succp = false;
        char** option;
        
        option = router->service->routerOptions;
        
        while (option != NULL)
        {
                char*  value;

                if ((value = strchr(options[i], '=')) == NULL)
                {
                        break;
                }
                else
                {
                        *value = 0;
                        value++;
                        if (strcmp(options[i], "slave_selection_criteria") == 0)
                        {
                                if (GET_SELECT_CRITERIA(value) == (select_criteria_t *)*data)
                                {
                                        succp = true;
                                        break;
                                }
                        }
                }
        }
        return succp;
}
#endif /*< NOT_USED */

/**
 * Error Handler routine to resolve _backend_ failures. If it succeeds then there
 * are enough operative backends available and connected. Otherwise it fails, 
 * and session is terminated.
 *
 * @param       instance        The router instance
 * @param       router_session  The router session
 * @param       errmsgbuf       The error message to reply
 * @param       backend_dcb     The backend DCB
 * @param       action          The action: REPLY, REPLY_AND_CLOSE, NEW_CONNECTION
 * @param       succp           Result of action. 
 * 
 * Even if succp == true connecting to new slave may have failed. succp is to
 * tell whether router has enough master/slave connections to continue work.
 */
static void handleError (
        ROUTER*        instance,
        void*          router_session,
        GWBUF*         errmsgbuf,
        DCB*           backend_dcb,
        error_action_t action,
        bool*          succp)
{
        SESSION*           session;
        ROUTER_INSTANCE*   inst    = (ROUTER_INSTANCE *)instance;
        ROUTER_CLIENT_SES* rses    = (ROUTER_CLIENT_SES *)router_session;
      
        CHK_DCB(backend_dcb);
	/** Don't handle same error twice on same DCB */
	if (backend_dcb->dcb_errhandle_called)
	{
		/** we optimistically assume that previous call succeed */
		*succp = true;
		return;
	}
	else
	{
		backend_dcb->dcb_errhandle_called = true;
	}
        session = backend_dcb->session;
        
        if (session == NULL || rses == NULL)
	{
                *succp = false;
		return;
	}
	CHK_SESSION(session);
	CHK_CLIENT_RSES(rses);
        
        switch (action) {
                case ERRACT_NEW_CONNECTION:
                {
                        if (!rses_begin_locked_router_action(rses))
                        {
                                *succp = false;
                                return;
                        }
			/**
			* This is called in hope of getting replacement for 
			* failed slave(s).
			*/
			*succp = handle_error_new_connection(inst, 
							rses, 
							backend_dcb, 
							errmsgbuf);
			rses_end_locked_router_action(rses);
                        break;
                }
                
                case ERRACT_REPLY_CLIENT:
                {
                        handle_error_reply_client(session, 
						  rses, 
						  backend_dcb, 
						  errmsgbuf);
			*succp = false; /*< no new backend servers were made available */
                        break;       
                }
                
                default:
                        *succp = false;
                        break;
        }
}


static void handle_error_reply_client(
	SESSION*           ses,
	ROUTER_CLIENT_SES* rses,
	DCB*               backend_dcb,
	GWBUF*             errmsg)
{
	session_state_t sesstate;
	DCB*            client_dcb;
	backend_ref_t*  bref;
	
	spinlock_acquire(&ses->ses_lock);
	sesstate = ses->state;
	client_dcb = ses->client;
	spinlock_release(&ses->ses_lock);

	/**
	 * If bref exists, mark it closed
	 */
	if ((bref = get_bref_from_dcb(rses, backend_dcb)) != NULL)
	{
		CHK_BACKEND_REF(bref);
		bref_clear_state(bref, BREF_IN_USE);
		bref_set_state(bref, BREF_CLOSED);
	}
	
	if (sesstate == SESSION_STATE_ROUTER_READY)
	{
		CHK_DCB(client_dcb);
		client_dcb->func.write(client_dcb, gwbuf_clone(errmsg));
	}
}

/**
 * Check if there is backend reference pointing at failed DCB, and reset its
 * flags. Then clear DCB's callback and finally try to reconnect.
 * 
 * This must be called with router lock. 
 * 
 * @param inst		router instance
 * @param rses		router client session
 * @param dcb		failed DCB
 * @param errmsg	error message which is sent to client if it is waiting
 * 
 * @return true if there are enough backend connections to continue, false if not
 */
static bool handle_error_new_connection(
	ROUTER_INSTANCE*   inst,
	ROUTER_CLIENT_SES* rses,
	DCB*               backend_dcb,
	GWBUF*             errmsg)
{
	SESSION*       ses;
	int            router_nservers;


	backend_ref_t* bref;
	bool           succp;
	
	ss_dassert(SPINLOCK_IS_LOCKED(&rses->rses_lock));
	
	ses = backend_dcb->session;
	CHK_SESSION(ses);
	
	/**
	 * If bref == NULL it has been replaced already with another one.
	 */
	if ((bref = get_bref_from_dcb(rses, backend_dcb)) == NULL)
	{
		ss_dassert(bref != NULL);
		succp = false;
		goto return_succp;
	}
	CHK_BACKEND_REF(bref);
	
	/** 
	 * If query was sent through the bref and it is waiting for reply from
	 * the backend server it is necessary to send an error to the client
	 * because it is waiting for reply.
	 */
	if (BREF_IS_WAITING_RESULT(bref))
	{
		DCB* client_dcb;
		client_dcb = ses->client;
		client_dcb->func.write(client_dcb, gwbuf_clone(errmsg));
		bref_clear_state(bref, BREF_WAITING_RESULT);
	}
	bref_clear_state(bref, BREF_IN_USE);
	bref_set_state(bref, BREF_CLOSED);

	/** 
	 * Error handler is already called for this DCB because
	 * it's not polling anymore. It can be assumed that
	 * it succeed because rses isn't closed.
	 */
	if (backend_dcb->state != DCB_STATE_POLLING)
	{
		succp = true;
		goto return_succp;
	}	
	/** 
	 * Remove callback because this DCB won't be used 
	 * unless it is reconnected later, and then the callback
	 * is set again.
	 */
	dcb_remove_callback(backend_dcb, 
			DCB_REASON_NOT_RESPONDING, 
			&router_handle_state_switch, 
			(void *)bref);
	
	router_nservers = router_get_servercount(inst);
	/** 
	 * Try to get replacement slave or at least the minimum 
	 * number of slave connections for router session.
	 */
	succp = connect_backend_servers(
			rses->rses_backend_ref,
			router_nservers,
			ses,
			inst);
	
return_succp:
	return succp;        
}


static void print_error_packet(
        ROUTER_CLIENT_SES* rses, 
        GWBUF*             buf, 
        DCB*               dcb)
{
#if defined(SS_DEBUG)
        if (GWBUF_IS_TYPE_MYSQL(buf))
        {
                while (gwbuf_length(buf) > 0)
                {
                        /** 
                         * This works with MySQL protocol only ! 
                         * Protocol specific packet print functions would be nice.
                         */
                        uint8_t* ptr = GWBUF_DATA(buf);
                        size_t   len = MYSQL_GET_PACKET_LEN(ptr);
                        
                        if (MYSQL_GET_COMMAND(ptr) == 0xff)
                        {
                                SERVER*        srv = NULL;
                                backend_ref_t* bref = rses->rses_backend_ref;
                                int            i;
                                char*          bufstr;
                                
                                for (i=0; i<rses->rses_nbackends; i++)
                                {
                                        if (bref[i].bref_dcb == dcb)
                                        {
                                                srv = bref[i].bref_backend->backend_server;
                                        }
                                }
                                ss_dassert(srv != NULL);
                                char* str = (char*)&ptr[7]; 
                                bufstr = strndup(str, len-3);
                                
                                LOGIF(LE, (skygw_log_write_flush(
                                        LOGFILE_ERROR,
                                        "Error : Backend server %s:%d responded with "
                                        "error : %s",
                                        srv->name,
                                        srv->port,
                                        bufstr)));                
                                free(bufstr);
                        }
                        buf = gwbuf_consume(buf, len+4);
                }
        }
        else
        {
                while ((buf = gwbuf_consume(buf, GWBUF_LENGTH(buf))) != NULL);
        }
#endif /*< SS_DEBUG */
}

static int router_get_servercount(
        ROUTER_INSTANCE* inst)
{
        int       router_nservers = 0;
        BACKEND** b = inst->servers;
        /** count servers */
        while (*(b++) != NULL) router_nservers++;
                                                                
        return router_nservers;
}

#if 0
/** 
 * Find out the number of read backend servers.
 * Depending on the configuration value type, either copy direct count 
 * of slave connections or calculate the count from percentage value.
 */
static int rses_get_max_slavecount(
        ROUTER_CLIENT_SES* rses,
        int                router_nservers)
{
        int conf_max_nslaves;
        int max_nslaves;
        
        CHK_CLIENT_RSES(rses);
        
        if (rses->rses_config.rw_max_slave_conn_count > 0)
        {
                conf_max_nslaves = rses->rses_config.rw_max_slave_conn_count;
        }
        else
        {
                conf_max_nslaves = 
                (router_nservers*rses->rses_config.rw_max_slave_conn_percent)/100;
        }
        max_nslaves = MIN(router_nservers-1, MAX(1, conf_max_nslaves));
        
        return max_nslaves;
}
#endif


/**
 * Finds out if there is a backend reference pointing at the DCB given as 
 * parameter. 
 * @param rses	router client session
 * @param dcb	DCB
 * 
 * @return backend reference pointer if succeed or NULL
 */
static backend_ref_t* get_bref_from_dcb(
        ROUTER_CLIENT_SES* rses,
        DCB*               dcb)
{
        backend_ref_t* bref;
        int            i = 0;
        CHK_DCB(dcb);
        CHK_CLIENT_RSES(rses);
        
        bref = rses->rses_backend_ref;
        
        while (i<rses->rses_nbackends)
        {
                if (bref->bref_dcb == dcb)
                {
                        break;
                }
                bref++;
                i += 1;
        }
        
        if (i == rses->rses_nbackends)
        {
                bref = NULL;
        }
        return bref;
}

/**
 * Calls hang-up function for DCB if it is not both running and in 
 * master/slave/joined/ndb role. Called by DCB's callback routine.
 */
static int router_handle_state_switch(
        DCB*       dcb,
        DCB_REASON reason,
        void*      data)
{
        backend_ref_t*     bref;
        int                rc = 1;
        ROUTER_CLIENT_SES* rses;
        SESSION*           ses;
        SERVER*            srv;
        
        CHK_DCB(dcb);
        bref = (backend_ref_t *)data;
        CHK_BACKEND_REF(bref);
       
        srv = bref->bref_backend->backend_server;
        
        if (SERVER_IS_RUNNING(srv) && SERVER_IS_IN_CLUSTER(srv))
        {
                goto return_rc;
        }
        ses = dcb->session;
        CHK_SESSION(ses);

        rses = (ROUTER_CLIENT_SES *)dcb->session->router_session;
        CHK_CLIENT_RSES(rses);

        switch (reason) {
                case DCB_REASON_NOT_RESPONDING:
                        dcb->func.hangup(dcb);
                        break;
                        
                default:
                        break;
        }
        
return_rc:
        return rc;
}


static sescmd_cursor_t* backend_ref_get_sescmd_cursor (
        backend_ref_t* bref)
{
        sescmd_cursor_t* scur;
        CHK_BACKEND_REF(bref);
        
        scur = &bref->bref_sescmd_cur;
        CHK_SESCMD_CUR(scur);
        
        return scur;
}

#if defined(PREP_STMT_CACHING)
#define MAX_STMT_LEN 1024

static prep_stmt_t* prep_stmt_init(
        prep_stmt_type_t type,
        void*            id)
{
        prep_stmt_t* pstmt;
        
        pstmt = (prep_stmt_t *)calloc(1, sizeof(prep_stmt_t));
        
        if (pstmt != NULL)
        {
#if defined(SS_DEBUG)
                pstmt->pstmt_chk_top  = CHK_NUM_PREP_STMT;
                pstmt->pstmt_chk_tail = CHK_NUM_PREP_STMT;
#endif
                pstmt->pstmt_state = PREP_STMT_ALLOC;
                pstmt->pstmt_type  = type;
                
                if (type == PREP_STMT_NAME)
                {
                        pstmt->pstmt_id.name = strndup((char *)id, MAX_STMT_LEN);
                }
                else
                {
                        pstmt->pstmt_id.seq = 0;
                }
        }
        CHK_PREP_STMT(pstmt);
        return pstmt;
}

static void prep_stmt_done(
        prep_stmt_t* pstmt)
{
        CHK_PREP_STMT(pstmt);
        
        if (pstmt->pstmt_type == PREP_STMT_NAME)
        {
                free(pstmt->pstmt_id.name);
        }
        free(pstmt);
}

static bool prep_stmt_drop(
        prep_stmt_t* pstmt)
{
        CHK_PREP_STMT(pstmt);
        
        pstmt->pstmt_state = PREP_STMT_DROPPED;
        return true;
}
#endif /*< PREP_STMT_CACHING */

/********************************
 * This routine returns the root master server from MySQL replication tree
 * Get the root Master rule:
 *
 * find server with the lowest replication depth level
 * and the SERVER_MASTER bitval
 * Servers are checked even if they are in 'maintenance'
 *
 * @param	servers		The list of servers
 * @param	router_nservers	The number of servers
 * @return			The Master found
 *
 */
static BACKEND *get_root_master(
	backend_ref_t *servers, 
	int            router_nservers) 
{
        int i = 0;
        BACKEND * master_host = NULL;

        for (i = 0; i< router_nservers; i++) 
	{
		BACKEND* b;
		
		if (servers[i].bref_backend == NULL)
		{
			continue;
		}
		
		b = servers[i].bref_backend;

		if ((b->backend_server->status & 
			(SERVER_MASTER|SERVER_MAINT)) == SERVER_MASTER) 
		{
			if (master_host == NULL || 
				(b->backend_server->depth < master_host->backend_server->depth))
			{
				master_host = b;
                        }
                }
        }
	return master_host;
}


/********************************
 * This routine returns the root master server from MySQL replication tree
 * Get the root Master rule:
 *
 * find server with the lowest replication depth level
 * and the SERVER_MASTER bitval
 * Servers are checked even if they are in 'maintenance'
 *
 * @param	rses pointer to router session
 * @return	pointer to backend reference of the root master or NULL
 *
 */
static backend_ref_t* get_root_master_bref(
	ROUTER_CLIENT_SES* rses)
{
	backend_ref_t* bref;
	backend_ref_t* candidate_bref = NULL;
	int            i = 0;
	
	bref = rses->rses_backend_ref;
	
	while (i<rses->rses_nbackends)
	{
		if ((bref->bref_backend->backend_server->status &
			(SERVER_MASTER|SERVER_MAINT)) == SERVER_MASTER)
		{
			if (bref->bref_backend->backend_server->status & SERVER_MASTER)
			{
				if (candidate_bref == NULL ||
					(bref->bref_backend->backend_server->depth <
					candidate_bref->bref_backend->backend_server->depth))
				{
					candidate_bref = bref;
				}
			}
		}
		bref++;
		i += 1;
	}
	if (candidate_bref == NULL)
	{
		LOGIF(LE, (skygw_log_write_flush(
			LOGFILE_ERROR,
			"Error : Could not find master among the backend "
			"servers. Previous master's state : %s",
			STRSRVSTATUS(BREFSRV(rses->rses_master_ref)))));	
	}
	return candidate_bref;
}


static void dbshard_process_router_options(
        ROUTER_INSTANCE* router,
        char**           options)
{
        int               i;
        char*             value;
        
        for (i = 0; options[i]; i++)
        {
                if ((value = strchr(options[i], '=')) == NULL)
                {
                        LOGIF(LE, (skygw_log_write(
                                LOGFILE_ERROR, "Warning : Unsupported "
                                "router option \"%s\" for "
                                "readwritesplit router.",
                                options[i])));
                }
                else
                {
                        *value = 0;
                        value++;
#if 0
                        if (strcmp(options[i], "slave_selection_criteria") == 0)
                        {
                                c = GET_SELECT_CRITERIA(value);
                                ss_dassert(
                                        c == LEAST_GLOBAL_CONNECTIONS ||
                                        c == LEAST_ROUTER_CONNECTIONS ||
                                        c == LEAST_BEHIND_MASTER ||
                                        c == LEAST_CURRENT_OPERATIONS ||
                                        c == UNDEFINED_CRITERIA);
                               
                                if (c == UNDEFINED_CRITERIA)
                                {
                                        LOGIF(LE, (skygw_log_write(
                                                LOGFILE_ERROR, "Warning : Unknown "
                                                "slave selection criteria \"%s\". "
                                                "Allowed values are LEAST_GLOBAL_CONNECTIONS, "
                                                "LEAST_ROUTER_CONNECTIONS, "
                                                "LEAST_BEHIND_MASTER,"
                                                "and LEAST_CURRENT_OPERATIONS.",
                                                STRCRITERIA(router->dbshard_config.rw_slave_select_criteria))));
                                }
                                else
                                {
                                        router->dbshard_config.rw_slave_select_criteria = c;
                                }
                        }
#endif
                }
        } /*< for */
}

/**
 * Read new database nbame from MYSQL_COM_INIT_DB packet, check that it exists
 * in the hashtable and copy its name to MYSQL_session.
 * 
 * @param inst	Router instance
 * @param rses	Router client session
 * @param buf	Query buffer
 * 
 * @return true if new database is set, false if non-existent database was tried
 * to be set
 */
static bool change_current_db(
	ROUTER_INSTANCE*   inst,
	ROUTER_CLIENT_SES* rses,
	GWBUF*             buf)
{
	bool	 succp;
	uint8_t* packet;
	int 	 message_len;
	char*	 fail_str;
	
	if(GWBUF_LENGTH(buf) <= MYSQL_DATABASE_MAXLEN - 5)
	{
		packet = GWBUF_DATA(buf);
		/** Copy database name from MySQL packet to session */
		strncpy(rses->rses_mysql_session->db,
			(char*)(packet + 5),
			(int)(GWBUF_LENGTH(buf) - 5));
		/**
		 * Update the session's active database only if it's in the hashtable.
		 * If it isn't found, send a custom error packet to the client.
		 */
		if(hashtable_fetch(
			inst->dbnames_hash,
			(char*)rses->rses_mysql_session->db) == NULL)
		{			
			/** Create error message */
			message_len = 25 + MYSQL_DATABASE_MAXLEN;
			fail_str = calloc(1, message_len+1);
			snprintf(fail_str, 
				message_len, 
				"Unknown database '%s'", 
				(char*)rses->rses_mysql_session->db);
			rses->rses_mysql_session->db[0] = '\0';			
			succp = false;
			goto reply_error;
		}
		else
		{
			succp = true;
			goto retblock;
		}
	}
	else 
	{
		/** Create error message */
		message_len = 25 + MYSQL_DATABASE_MAXLEN;
		fail_str = calloc(1, message_len+1);
		snprintf(fail_str, 
			message_len, 
			"Unknown database '%s'", 
			(char*)rses->rses_mysql_session->db);
		succp = false;
		goto reply_error;
	}
reply_error:
	{
		GWBUF* errbuf;
		errbuf = modutil_create_mysql_err_msg(2, 0, 1049, "42000", fail_str);
		free(fail_str);
		
		if (errbuf == NULL)
		{
			LOGIF(LE, (skygw_log_write_flush(
				LOGFILE_ERROR,
				"Error : Creating buffer for error message failed."))); 
			goto retblock;			
		}
		/** Set flags that help router to identify session commans reply */
		gwbuf_set_type(errbuf, GWBUF_TYPE_MYSQL);
		gwbuf_set_type(errbuf, GWBUF_TYPE_SESCMD_RESPONSE);
		gwbuf_set_type(errbuf, GWBUF_TYPE_RESPONSE_END);
		/** 
		* Create an incoming event for randomly selected backend DCB which
		* will then be notified and replied 'back' to the client.
		*/
		poll_add_epollin_event_to_dcb(rses->rses_backend_ref->bref_dcb, 
					gwbuf_clone(errbuf));
		gwbuf_free(errbuf);
	}
retblock:
	return succp;
}
/**
 * Parses the configuration for databases to ignore.
 * @param router The router instance
 * @param param Configuration parameters
 * @return True if the parsing was successful and false if an error occurred.
 */
bool parse_db_ignore_list(ROUTER_INSTANCE* router, char* param)
{
	char** list = router->ignore_list;
	int count = 0, i = 0;
	char *value = param, *tok;
	char **saveptr = NULL,**tmp;

	list = malloc(sizeof(char*));

	if(list == NULL){
		LOGIF(LE, (skygw_log_write_flush(
			LOGFILE_ERROR,
			"Error : malloc returned NULL.")));
		return false;
	}

	if(param == NULL)
	{
		list[0] = NULL;
		return true;
	}

	/**We have at least one value to ignore*/

	count++;

	tok = value;

	while((tok = strchr(tok,',')))
	{
		tok++;
		count++;
	}

    tmp = realloc(list,(count + 1)*sizeof(char*));
	tok = strtok_r(value,",",saveptr);
	
	if(tmp == NULL)
	{
		LOGIF(LE, (skygw_log_write_flush(
			LOGFILE_ERROR,
			"Error : realloc returned NULL.")));
		return false;
	}

	list = tmp;

	while(tok && count > i)
	{
		list[i++] = strdup(tok);
		tok = strtok_r(NULL,",",saveptr);
	}

	list[i] = NULL;
	return true;
}
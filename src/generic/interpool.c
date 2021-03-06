/*
 * interpool.c -- Interpreter Pool Manager for Apache Module
 * nca-073-9
 *
 * Copyright (c) 1996-2000 by Netcetera AG.
 * Copyright (c) 2001 by Apache Software Foundation.
 * All rights reserved.
 *
 * See the file "license.terms" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * @(#) $Id: interpool.c 794247 2009-07-15 12:15:16Z ronnie $
 *
 */

#include "hashutl.h"
#include "interpool.h"
#include "log.h"
#include "log.h"
#include "logtoap.h"
#include "macros.h"
#include "modwebsh.h"
#include "tcl.h"
#include "webutl.h"
#include "web.h"
#include "mod_websh.h"
#include "request.h"
#include <time.h>
#include <sys/stat.h>
#ifndef WIN32
#include <unistd.h>
#else
/* define access mode constants if they are not defined yet
   if we're under Windows (they are defined in unistd.h, which doesn't
   exist under Windows) currently only R_OK is used.
 */
#ifndef R_OK
#define R_OK 4
#endif
#endif

#include <tcl.h>
#include <assert.h>

#define TCL_TSD_INIT(keyPtr) \
  ((ThreadSpecificData *)Tcl_GetThreadData((keyPtr), sizeof(ThreadSpecificData)))

typedef struct ThreadSpecificData {
    websh_server_conf *conf;
    Tcl_Interp        *mainInterp;
    WebshPool         *webshPool;
} ThreadSpecificData;

static Tcl_ThreadDataKey dataKey;


/* init script for main interpreter */

#define MAININTERP_INITCODE "proc web::interpmap {filename} {return $filename}"

#define WIP_CURRENT_THREAD 0
#define WIP_CHECK_THREAD   1
#define WIP_FORCE_REMOVE   2

/* #define DEBUG 1 */

#ifdef DEBUG
  static char debug_trace_message[4096];
  #define DEBUG_TRACE(server, ...)  ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, server, __VA_ARGS__);
  #define DEBUG_TRACE2(server, ...) sprintf(debug_trace_message, __VA_ARGS__); ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, server, debug_trace_message);

  #ifndef APACHE2
    #define AP_LOG_DEBUG(server, ...) ap_log_printf(server, __VA_ARGS__);
  #else /* APACHE2 */
    #define AP_LOG_DEBUG(server, ...) ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, __VA_ARGS__);
  #endif
#else
  #define DEBUG_TRACE(server, ...)
  #define DEBUG_TRACE2(server, ...)
  #define AP_LOG_DEBUG(server, ...)
#endif


#ifndef APACHE2
  #define AP_LOG_ERROR(server, ...) ap_log_printf(server, __VA_ARGS__);
  #define AP_LOG_RERROR(r, ...) ap_log_rerror(APLOG_MARK, APLOG_ERR, r, __VA_ARGS__);
#else /* APACHE2 */
  #define AP_LOG_ERROR(server, format, ...) ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, format, ##__VA_ARGS__);
  #define AP_LOG_RERROR(r, format, ...) ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, 0, r, format, ##__VA_ARGS__);
#endif


/* ----------------------------------------------------------------------------
 * Declaration
 * ------------------------------------------------------------------------- */

static Tcl_Interp *createMainInterp(websh_server_conf * conf);
static int initMainInterp(websh_server_conf * conf, Tcl_Interp *mainInterp);

static WebInterp *poolCreateWebInterp(
           websh_server_conf * conf,
           WebInterpClass * webInterpClass,
           char *filename, long mtime,
           request_rec *r);
static void poolDestroyWebInterp(WebInterp * webInterp, int flag);

// static WebInterpClass *poolCreateWebInterpClass(websh_server_conf * conf, Tcl_HashTable *webshPool, char *filename, long mtime);
static int             poolDestroyWebInterpClass(WebInterpClass * webInterpClass);

static WebInterpClass *updateWebInterpClass(
   WebInterpClass *webInterpClass,
   const char *newfile, long mtime
);


static int readWebInterpClassCode(WebInterp *webInterp, char *filename, long mtime);
// static void deleteInterpClass(WebInterpClass * webInterpClass);


/* ----------------------------------------------------------------------------
 * reserve/release WebInterp
 * ------------------------------------------------------------------------- */

static void reserveWebInterp(WebInterp *webInterp){
    if ( webInterp==NULL ) return;

    webInterp->state = WIP_INUSE;
}

static void releaseWebInterp(WebInterp *webInterp){
    WebInterpClass *webInterpClass = webInterp->interpClass;

    webInterp->lastusedtime = (long) time(NULL);
    webInterp->numrequests++;

    switch(webInterp->state){
	case WIP_EXPIRED:
            break;
	case WIP_EXPIRED_INUSE :
	    webInterp->state = WIP_EXPIRED;
	    break;
	case WIP_INUSE:
	    webInterp->state = WIP_FREE;
            AP_LOG_DEBUG(webInterp->req->server, "release interpreter to WIP_FREE %% id = %ld , class = %s", webInterp->id, webInterp->interpClass->filename);
	    if (webInterpClass->maxrequests && (webInterp->numrequests >= webInterpClass->maxrequests)) {
		logToAp(webInterp->interp, NULL,
			"interpreter expired: request count reached (id %ld, class %s)", webInterp->id, webInterp->interpClass->filename);
		webInterp->state = WIP_EXPIRED;
	    }
            break;
	default :
	    AP_LOG_ERROR(webInterp->req->server,
		     "unexpected interpreter state when release id = %ld , class = %s", webInterp->id, webInterp->interpClass->filename);
    }

    if( webInterp->state == WIP_EXPIRED ){
        poolDestroyWebInterp(webInterp, WIP_CURRENT_THREAD);
    }

    return;
}



/*
*  WIP_FREE -> WIP_INUSE -> WIP_FREE -> WIP_EXPIRED
*                        -> WIP_EXPIRED_INUSE
*/
static WebInterp *poolGetFreeWebInterp(WebInterpClass *webInterpClass)
{
    /* search a free interp */

    if( webInterpClass->first == NULL) {
      return NULL;
    }

    WebInterp *found = NULL;
    Tcl_ThreadId current_thread;
    time_t t;

    time(&t);
    current_thread = Tcl_GetCurrentThread();

    WebInterp *webInterp;
    for(webInterp = webInterpClass->first; webInterp != NULL; webInterp = webInterp->next) {
	if (webInterp->originThrdId != current_thread) continue;  // check thread owner

	if (webInterp->state == WIP_FREE) {
            logToAp(webInterp->interp, NULL, "poolGetFreeWebInterp in current thread");
	    if (webInterpClass->maxidletime && (t - webInterp->lastusedtime) > webInterpClass->maxidletime) {
		logToAp(webInterp->interp, NULL,
			"interpreter expired: idle time reached (id %ld, claa %s)", webInterp->id, webInterp->interpClass->filename);
		webInterp->state = WIP_EXPIRED;

	    } else if (webInterpClass->maxttl && (t - webInterp->starttime) > webInterpClass->maxttl) {
		logToAp(webInterp->interp, NULL,
		        "interpreter expired: time to live reached (id %ld, class %s)", webInterp->id, webInterp->interpClass->filename);
		webInterp->state = WIP_EXPIRED;
                logToAp(webInterp->interp, NULL, "poolGetFreeWebInterp mark WIP_EXPIRED due to maxttl");
            } else {
                logToAp(webInterp->interp, NULL, "poolGetFreeWebInterp found free WebInterp");
	        found = webInterp;
	        break;
	    }
	}
    }

    return found;
}

/* ----------------------------------------------------------------------------
 * Thread specific inter pool
 *
 * For thread in Tcl:
 *   1. Each thread can have multiple interpeter.
 *   2. But each interpeter can only belong to one specific thread.
 *   3. Access interpeter created by other threads is not allowed.
 *
 * As a result, the WebInterp pool must be thread specific.
 * In other words, each thread should have and use its own webshPool.
 *
 * Struct `ThreadSpecificData` is used here for implementation.
 * See http://www.tcl.tk/doc/howto/thread_model.html about it.
 * ------------------------------------------------------------------------- */

static void initPoolThread(websh_server_conf * conf, apr_thread_t *current_thread)
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    if(tsdPtr->conf != NULL) return;

    DEBUG_TRACE(conf->server, "initPoolThread");

    tsdPtr->conf = conf;
    tsdPtr->mainInterp = createMainInterp(conf);
    HashUtlAllocInit(tsdPtr->webshPool, TCL_STRING_KEYS);

    apr_thread_data_set(conf, "WebInterpThreadPool", destroyPoolThread, current_thread);
}

static apr_status_t destroyPoolThread(void *data)
{
    websh_server_conf *conf = (websh_server_conf *) data;

    if (conf == NULL)
        return APR_SUCCESS;

    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    if( tsdPtr->webshPool==NULL ) {
        return APR_SUCCESS;
    }

    DEBUG_TRACE(conf->server, "destroyPoolThread");

    Tcl_HashEntry *entry;
    Tcl_HashSearch search;

    while ((entry = Tcl_FirstHashEntry(tsdPtr->webshPool, &search)) != NULL) {
	/* loop through entries */
	poolDestroyWebInterpClass((WebInterpClass *) Tcl_GetHashValue(entry));
	Tcl_DeleteHashEntry(entry);

        DEBUG_TRACE(conf->server, "thread poolDestroyWebInterpClass ok");
    }

    Tcl_DeleteHashTable(tsdPtr->webshPool);
    tsdPtr->webshPool = NULL;
    DEBUG_TRACE(conf->server, "destroyPoolThread ok");

    return APR_SUCCESS;
}

WebInterp *poolGetThreadWebInterp(websh_server_conf *conf, char *filename,
			    long mtime, request_rec * r)
{

    initPoolThread(conf, r->connection->current_thread);

    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    WebInterpClass *webInterpClass;
    WebInterp      *webInterp = NULL;

    webInterpClass = poolCreateWebInterpClass(conf, tsdPtr->webshPool, filename, mtime);
    if(webInterpClass==NULL){
       AP_LOG_ERROR(r->server, "cannot access or stat webInterpClass file '%s'", filename);
       return NULL;
    }

    AP_LOG_DEBUG(r->server, "use WebInterpClass maxrequests=%ld maxttl=%ld maxidletime=%ld mtime=%ld from %s",
        webInterpClass->maxrequests, webInterpClass->maxttl, webInterpClass->maxidletime,
        webInterpClass->mtime, webInterpClass->filename);

    webInterp = poolGetFreeWebInterp(webInterpClass);

    if( webInterp==NULL ) {
        AP_LOG_DEBUG(r->server, "create new WebInterp %ld %s", mtime, filename);
	webInterp = poolCreateWebInterp(conf, webInterpClass, filename, mtime, r);
    }

    if (webInterp == NULL){
	AP_LOG_RERROR(r, "mod_websh - no web interp!");
	return NULL; 
    } else if (webInterp->state != WIP_FREE) {
        expireWebInterp(webInterp);
        AP_LOG_RERROR(r, "unexpected interpreter state != WIP_FREE %% id = %ld , class = %s", webInterp->id, webInterp->interpClass->filename);
        return NULL;
    } else if (webInterp->interp == NULL) {
        expireWebInterp(webInterp);
	AP_LOG_RERROR(r, "mod_websh - null interp!");
        
	return NULL;
    } else if (webInterp->code == NULL) {
        expireWebInterp(webInterp);
	AP_LOG_RERROR(r, "mod_websh - interp code is null!");
	return NULL;
    } else if (Tcl_InterpDeleted(webInterp->interp)) {
        expireWebInterp(webInterp);
	AP_LOG_RERROR(r, "mod_websh - hey, somebody is deleting the interp!");

	return NULL;
    }

    AP_LOG_DEBUG(r->server, "reserve WebInterp #%ld %ld/%ld of class %ld %s",
       webInterp->id,
       webInterp->numrequests, webInterp->interpClass->maxrequests,
       webInterp->interpClass->mtime,
       webInterp->interpClass->filename
    );

    webInterp->req = r;
    reserveWebInterp(webInterp);

    return webInterp;
}

void poolReleaseThreadWebInterp(WebInterp * webInterp)
{
    WebInterpClass *webInterpClass = webInterp->interpClass;

    releaseWebInterp(webInterp);

    cleanupPool(webInterpClass->webshPool);
}

/* ----------------------------------------------------------------------------
 * poolCreateWebInterpClass
 * ------------------------------------------------------------------------- */
static inline WebInterpClass *poolLookupWebInterpClass(Tcl_HashTable *webshPool, char *filename){
    WebInterpClass *webInterpClass = NULL;

    Tcl_HashEntry *entry;

    entry = Tcl_FindHashEntry(webshPool, filename);
    if (entry == NULL) {
        return NULL;
    }

    webInterpClass = (WebInterpClass *) Tcl_GetHashValue(entry);
    return webInterpClass;
}

WebInterpClass *poolCreateWebInterpClass(
    websh_server_conf * conf,
    Tcl_HashTable *webshPool,
    char *filename,
    long mtime
)
{
    WebInterpClass *webInterpClass = NULL;

    webInterpClass = poolLookupWebInterpClass(webshPool, filename);

    if (webInterpClass != NULL) {
	/* check if mtime is ok */
        webInterpClass = updateWebInterpClass(webInterpClass, filename, mtime);

        if (webInterpClass != NULL) {
          return webInterpClass;
        } else {
          /* TODO: destroy invalid webInterpClass */
        }
    }

    webInterpClass = WebAllocInternalData(WebInterpClass);

    if (webInterpClass != NULL) {

        if (mtime<=0) {
	  struct stat statPtr;
	  Tcl_Stat(filename, &statPtr);
          mtime = statPtr.st_mtime;
        }

	webInterpClass->webshPool = webshPool;
	webInterpClass->conf = conf;
	webInterpClass->filename = allocAndSet(filename);

	webInterpClass->maxrequests = 1L;
	webInterpClass->maxttl = 0L;
	webInterpClass->maxidletime = 0L;

	webInterpClass->mtime = mtime;

	webInterpClass->nextid = 0;

	webInterpClass->first = NULL;
	webInterpClass->last = NULL;

	webInterpClass->code = NULL;	/* will be loaded on demand by first interp */

	int isnew = 0;
	Tcl_HashEntry *entry;
	entry = Tcl_CreateHashEntry(webshPool, filename, &isnew);
	/* isnew must be 1, since we searched already */
        assert(isnew==1);
	Tcl_SetHashValue(entry, (ClientData) webInterpClass);
    } else {

	// Tcl_MutexUnlock(&(conf->webshPoolLock));
        #ifndef APACHE2
	ap_log_printf(conf->server,
		      "cannot create webInterpClass '%s'", filename);
        #else /* APACHE2 */
	ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, conf->server,
		     "cannot create webInterpClass '%s'", filename);
        #endif /* APACHE2 */
	return NULL;
    }

    return webInterpClass;
}

/* ----------------------------------------------------------------------------
 * poolDestroyWebInterpClass
 * ------------------------------------------------------------------------- */
int poolDestroyWebInterpClass(WebInterpClass * webInterpClass)
{

    if (webInterpClass == NULL)
	return TCL_ERROR;

    while ((webInterpClass->first) != NULL) {
	poolDestroyWebInterp(webInterpClass->first, WIP_FORCE_REMOVE);
    }

    if (webInterpClass->code != NULL) {
	Tcl_DecrRefCount(webInterpClass->code);
    }

    Tcl_Free(webInterpClass->filename);
    Tcl_Free((char *) webInterpClass);

    return TCL_OK;
}


static apr_status_t threadReleasePool(void *data)
{
    websh_server_conf *conf = (websh_server_conf *) data;

    if (conf == NULL)
	return APR_SUCCESS;

    DEBUG_TRACE(conf->server, "releaseThreadPool in thread %ld", Tcl_GetCurrentThread());

    if (conf->webshPool != NULL) {
	Tcl_HashEntry *entry;
	Tcl_HashSearch search;

	Tcl_MutexLock(&(conf->webshPoolLock));
	entry = Tcl_FirstHashEntry(conf->webshPool, &search);
	while ( entry != NULL ) {
	    /* loop through entries */
	    WebInterpClass *webInterpClass = (WebInterpClass *) Tcl_GetHashValue(entry);
            WebInterp *webInterp = webInterpClass->first;
            while(webInterp != NULL) {
                WebInterp *nextWebInterp = webInterp->next;
                poolDestroyWebInterp(webInterp, WIP_CHECK_THREAD);
                webInterp = nextWebInterp;
            }

	    entry = Tcl_NextHashEntry(&search);
	}
	Tcl_MutexUnlock(&(conf->webshPoolLock));
    }

    return APR_SUCCESS;
}


static apr_status_t hookReleaseThreadPool(websh_server_conf *conf, request_rec *r)
{
  apr_thread_t *current_thread = r->connection->current_thread;

  apr_thread_data_set(conf, "websh_server_conf", threadReleasePool, current_thread);

  return APR_SUCCESS;
}

/* ----------------------------------------------------------------------------
 * poolCreateWebInterp
 * ------------------------------------------------------------------------- */

#define DOUBLE_LIST_PREPEND(node, head, tail) \
	node->prev=NULL ; node->next = head ; \
	if(head!=NULL) head->prev=node; \
	if(tail==NULL) tail = node; \
	head = node;

static WebInterp *poolCreateWebInterp(websh_server_conf * conf,
			   WebInterpClass * webInterpClass, char *filename,
			   long mtime, request_rec *r)
{

    int result = 0;
    LogPlugIn *logtoap = NULL;
    Tcl_Obj *code = NULL;
    ApFuncs *apFuncs = NULL;

    apFuncs = Tcl_GetAssocData(conf->mainInterp, WEB_APFUNCS_ASSOC_DATA, NULL);
    if (apFuncs == NULL){
	return NULL;
    }

    WebInterp *webInterp = (WebInterp *) Tcl_Alloc(sizeof(WebInterp));

    webInterp->interp = Tcl_CreateInterp();

    DEBUG_TRACE2(conf->server, "createWebInterp %p in thread %ld", webInterp->interp, Tcl_GetCurrentThread());

    if (webInterp->interp == NULL) {
	Tcl_Free((char *) webInterp);
	AP_LOG_RERROR(r, "createWebInterp: Could not create interpreter (id %ld, class %s)", webInterpClass->nextid, filename);
	return NULL;
    }

    // Tcl_Preserve(webInterp->interp);

    /* just to be sure the memory command is imported if
       the corresponding Tcl features it */
#ifdef TCL_MEM_DEBUG
    Tcl_InitMemory(webInterp->interp);
#endif

    /* now register here all websh modules */
    result = Tcl_Init(webInterp->interp);
    /* checkme: test result */

    webInterp->interpClass = webInterpClass;
    webInterp->code = NULL;

    if (webInterpClass->code == NULL) {
        /* XXX: Avoid using Tcl_Interp from other threads.
        **      Use Tcl_Interp from current webInterp.
        **      And load code into webInterp->interpClass->code
        */
	if (readWebInterpClassCode(webInterp, filename, mtime) != TCL_OK) {
	    AP_LOG_RERROR(r, "Failed not read code from %s: %s", filename,
                Tcl_GetStringResult(webInterp->interp));
	}
    }
    if (webInterpClass->code != NULL) {
	/* copy code from class */
	webInterp->code = webInterpClass->code;
	Tcl_IncrRefCount(webInterpClass->code);
    }
    if (webInterp->code == NULL){
	AP_LOG_RERROR(r, "debug: mod_websh - WebInterp code is null, delete interp");
        Tcl_DeleteInterp(webInterp->interp);
	Tcl_Free((char *) webInterp);
	return NULL;
    }

    Tcl_SetAssocData(webInterp->interp, WEB_APFUNCS_ASSOC_DATA, NULL, (ClientData *) apFuncs);

    result = Websh_Init(webInterp->interp);

    /* also register the destrcutor, etc. functions, passing webInterp as
       client data */

    /* ------------------------------------------------------------------------
     * register log handler "apachelog"
     * --------------------------------------------------------------------- */
    logtoap = createLogPlugIn();
    if (logtoap == NULL){
	AP_LOG_RERROR(r, "debug: mod_websh - createLogPlugIn fail");
        Tcl_DeleteInterp(webInterp->interp);
	Tcl_Free((char *) webInterp);
	return NULL;
    }

    logtoap->constructor = createLogToAp;
    logtoap->destructor = destroyLogToAp;
    logtoap->handler = (int (*)(Tcl_Interp *, ClientData, char *)) logToAp;
    registerLogPlugIn(webInterp->interp, APCHANNEL, logtoap);

    /* ------------------------------------------------------------------------
     * create commands
     * --------------------------------------------------------------------- */
    Tcl_CreateObjCommand(webInterp->interp, "web::initializer",
			 Web_Initializer, (ClientData) webInterp, NULL);

    Tcl_CreateObjCommand(webInterp->interp, "web::finalizer",
			 Web_Finalizer, (ClientData) webInterp, NULL);

    Tcl_CreateObjCommand(webInterp->interp, "web::finalize",
			 Web_Finalize, (ClientData) webInterp, NULL);

    Tcl_CreateObjCommand(webInterp->interp, "web::maineval",
			 Web_MainEval, (ClientData) webInterp, NULL);

    Tcl_CreateObjCommand(webInterp->interp, "web::interpcfg",
			 Web_InterpCfg, (ClientData) webInterp, NULL);

    Tcl_CreateObjCommand(webInterp->interp, "web::interpclasscfg",
			 Web_InterpClassCfg, (ClientData) webInterp, NULL);

    /* ------------------------------------------------------------------------
     * rename exit !
     * --------------------------------------------------------------------- */
    Tcl_Obj *initCode =
	Tcl_NewStringObj
	("rename exit exit.apache; proc exit {} {if {[info tclversion] >= 8.5} {return -level [expr {[info level] + 1}]} else {return -code error \"cannot exit script in mod_websh because process would terminate (use Tcl 8.5 or later if you want to use exit)\"}}", -1);

    Tcl_IncrRefCount(initCode);
    Tcl_EvalObjEx(webInterp->interp, initCode, 0);
    Tcl_DecrRefCount(initCode);
    Tcl_ResetResult(webInterp->interp);

    time_t t;
    time(&t);

    webInterp->dtor = NULL;
    webInterp->state = WIP_FREE;
    webInterp->numrequests = 0;
    webInterp->starttime    = t;
    webInterp->lastusedtime = t;
    webInterp->id = webInterpClass->nextid++;
    webInterp->originThrdId = Tcl_GetCurrentThread();

    /* add to beginning of list of webInterpClass */
    DOUBLE_LIST_PREPEND(webInterp, webInterpClass->first, webInterpClass->last);

    return webInterp;
}


/* ----------------------------------------------------------------------------
 * poolDestroyWebInterp
 * ------------------------------------------------------------------------- */
static void removeWebInterp(WebInterp * webInterp)
{
    /* --------------------------------------------------------------------------
     * fixup list linkage
     * ----------------------------------------------------------------------- */
    if (webInterp->prev != NULL)
	/* we are not the first */
	webInterp->prev->next = webInterp->next;
    else
	webInterp->interpClass->first = webInterp->next;

    if (webInterp->next != NULL)
	/* we are not the last */
	webInterp->next->prev = webInterp->prev;
    else
	webInterp->interpClass->last = webInterp->prev;

    return;
}

static void poolDestroyWebInterp(WebInterp * webInterp, int flag)
{
    request_rec *r;

    if (webInterp->originThrdId != Tcl_GetCurrentThread()) {
      DEBUG_TRACE2(webInterp->interpClass->conf->server, "skip poolDestroyWebInterp %p", webInterp->interp);
      if( flag == WIP_FORCE_REMOVE ) {
        removeWebInterp(webInterp);
      }
      return;
    }

    DEBUG_TRACE2(webInterp->interpClass->conf->server, "poolDestroyWebInterp %p", webInterp->interp);

    if (webInterp->dtor != NULL) {

	int result;

	result = Tcl_Eval(webInterp->interp, "web::finalize");

	if (result != TCL_OK) {
	    r = (request_rec *) Tcl_GetAssocData(webInterp->interp, WEB_AP_ASSOC_DATA, NULL);
#ifndef APACHE2
	    ap_log_printf(r->server,
			 "web::finalize error: %s",
			 Tcl_GetStringResult(webInterp->interp));
#else /* APACHE2 */
	    ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
			 "web::finalize error: %s",
			 Tcl_GetStringResult(webInterp->interp));
#endif /* APACHE2 */
	}

	Tcl_ResetResult(webInterp->interp);
    }

    /* --------------------------------------------------------------------------
     * cleanup code cache
     * ----------------------------------------------------------------------- */
    if (webInterp->dtor) {
	Tcl_DecrRefCount(webInterp->dtor);
	/* webInterp->dtor = NULL; */
    }
    if (webInterp->code) {
	Tcl_DecrRefCount(webInterp->code);
	webInterp->code = NULL;
    }

    Tcl_DeleteInterp(webInterp->interp);
    // Tcl_Release(webInterp->interp);

    removeWebInterp(webInterp);

    DEBUG_TRACE2(webInterp->interpClass->conf->server, "poolDestroyWebInterp ok");

    Tcl_Free((char *) webInterp);

    return;
}

static Tcl_Obj *mapWebInterpClass(char *filename, Tcl_Interp *mainInterp){
    Tcl_Obj *cmdList[2];
    Tcl_Obj *mapCmd = NULL;
    int res;

    cmdList[0] = Tcl_NewStringObj("web::interpmap", -1);
    cmdList[1] = Tcl_NewStringObj(filename, -1);
    Tcl_IncrRefCount(cmdList[0]);
    Tcl_IncrRefCount(cmdList[1]);
    mapCmd = Tcl_NewListObj(2, cmdList);
    Tcl_IncrRefCount(mapCmd);
    res = Tcl_EvalObjEx(mainInterp, mapCmd, 0);
    Tcl_DecrRefCount(mapCmd);
    Tcl_DecrRefCount(cmdList[0]);
    Tcl_DecrRefCount(cmdList[1]);

    if (res != TCL_OK) {
	return NULL;
    }

    Tcl_Obj *idObj = Tcl_DuplicateObj(Tcl_GetObjResult(mainInterp));
    Tcl_IncrRefCount(idObj);
    Tcl_ResetResult(mainInterp);

    return idObj;
}


static WebInterpClass *updateWebInterpClass(
   WebInterpClass *webInterpClass,
   const char *newfile, long mtime
)
{
    const char *oldfile = webInterpClass->filename;

    /* get last modified time for id */
    if ( newfile!=oldfile && strcmp(newfile, oldfile) ) {
	struct stat statPtr;
	if (Tcl_Access(newfile, R_OK) != 0 ||
 	    Tcl_Stat(newfile, &statPtr) != TCL_OK)
	{
	    return NULL;
	}
	mtime = statPtr.st_mtime;
    }

    if (mtime > webInterpClass->mtime) {
        /* invalidate all interpreters, code must be loaded from scratch */
        WebInterp *webInterp;
        for(webInterp = webInterpClass->first ; webInterp!= NULL ; webInterp = webInterp->next) {

	    logToAp(webInterp->interp, NULL,
		    "interpreter expired: source changed (id %ld, class %s)", webInterp->id, webInterp->interpClass->filename);
	    if (webInterp->state == WIP_INUSE){
		webInterp->state = WIP_EXPIRED_INUSE;
	    }else{
		webInterp->state = WIP_EXPIRED;
	    }
        }
        /* free code (will be loaded on demand) */
        if (webInterpClass->code) {
	    Tcl_DecrRefCount(webInterpClass->code);
	    webInterpClass->code = NULL;
        }
    }

    return webInterpClass;
}


/* ----------------------------------------------------------------------------
 * poolGetWebInterp - request new interp from pool
 * ------------------------------------------------------------------------- */
WebInterp *poolGetWebInterp(websh_server_conf * conf, char *filename,
			    long mtime, request_rec * r)
{

    Tcl_HashEntry *entry = NULL;
    WebInterp *found = NULL;
    WebInterpClass *webInterpClass = NULL;
    char *id = NULL;
    Tcl_Obj *idObj = NULL;
    Tcl_Obj *mapCmd = NULL;
    Tcl_Obj *cmdList[2];
    int res;

    /* get interpreter id for filename */

    Tcl_MutexLock(&(conf->mainInterpLock));

    idObj = mapWebInterpClass(filename, conf->mainInterp);

    if (idObj==NULL) {
        #ifndef APACHE2
	ap_log_printf(r->server,
		      "web::interpmap: %s",
		      Tcl_GetStringResult(conf->mainInterp));
        #else /* APACHE2 */
	ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
		      "web::interpmap: %s",
		      Tcl_GetStringResult(conf->mainInterp));
        #endif /* APACHE2 */

	/* no valid id for filename */
	Tcl_MutexUnlock(&(conf->mainInterpLock));
        return NULL;
    }

    /* get absolute filename (if already absolute, same as
       Tcl_GetString(idObj) -> no DecrRefCount yet) */
    id = (char *) ap_server_root_relative(r->pool, Tcl_GetString(idObj));

    DEBUG_TRACE(conf->server, "web::interpmap %s -> %s", filename, id);

    Tcl_MutexUnlock(&(conf->mainInterpLock));

    Tcl_MutexLock(&(conf->webshPoolLock));

    /* see if we have that id */
    entry = Tcl_FindHashEntry(conf->webshPool, id);
    if (entry != NULL) {

	/* yes, we have such a class */
	WebInterp *webInterp = NULL;

	webInterpClass = (WebInterpClass *) Tcl_GetHashValue(entry);

	/* check if mtime is ok */
        webInterpClass = updateWebInterpClass(webInterpClass, id, mtime);

        if( webInterpClass==NULL ){

            #ifndef APACHE2
	    ap_log_printf(r->server,
			  "cannot access or stat webInterpClass file '%s'", id);
            #else /* APACHE2 */
	    ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
			 "cannot access or stat webInterpClass file '%s'", id);
            #endif /* APACHE2 */

	    Tcl_DecrRefCount(idObj);
            return NULL;
        }

        webInterp = poolGetFreeWebInterp(webInterpClass);
        found = webInterp;
    } else {

	/* no, we have to create this new interpreter class */

	webInterpClass = poolCreateWebInterpClass(conf, conf->webshPool, id, mtime);

	if (webInterpClass == NULL) {
	    Tcl_MutexUnlock(&(conf->webshPoolLock));
	    Tcl_DecrRefCount(idObj);
	    return NULL;
	}
    }

    if (found == NULL) {
	/* we have to create one */
	found = poolCreateWebInterp(conf, webInterpClass, id, mtime, r);
    }

    DEBUG_TRACE2(conf->server, "poolGetWebInterp %p in thread %ld", found, Tcl_GetCurrentThread());

    if (found != NULL) {
	/* mark the found one as INUSE */
	found->state = WIP_INUSE;
    }

    Tcl_MutexUnlock(&(conf->webshPoolLock));
    Tcl_DecrRefCount(idObj);
    return found;
}

/* ----------------------------------------------------------------------------
 * poolReleaseWebInterp - bring intrp back into pool
 * ------------------------------------------------------------------------- */
void poolReleaseWebInterp(WebInterp * webInterp)
{

    if (webInterp != NULL) {

	WebInterpClass *webInterpClass = webInterp->interpClass;

	Tcl_MutexLock(&(webInterpClass->conf->webshPoolLock));

	webInterp->lastusedtime = (long) time(NULL);

	webInterp->numrequests++;

        DEBUG_TRACE(webInterpClass->conf->server, "numrequests = %d / %d , state = %d",
                webInterp->numrequests, webInterpClass->maxrequests, webInterp->state);

	if (webInterp->state == WIP_EXPIRED_INUSE) {
	    webInterp->state = WIP_EXPIRED;
	} else if (webInterp->state == WIP_EXPIRED) {
	    /* keep the state */
	} else {
	    webInterp->state = WIP_FREE;

	    /* check for num requests */

	    if (webInterpClass->maxrequests && (webInterp->numrequests >= webInterpClass->maxrequests)) {
		logToAp(webInterp->interp, NULL,
			"interpreter expired: request count reached (id %ld, class %s)", webInterp->id, webInterp->interpClass->filename);
		webInterp->state = WIP_EXPIRED;
	    }
	}

	/* cleanup all EXPIRED interps */
	cleanupPool(webInterpClass->conf->webshPool);

	Tcl_MutexUnlock(&(webInterpClass->conf->webshPoolLock));

    }
}


/* ----------------------------------------------------------------------------
 * init
 * ------------------------------------------------------------------------- */
int initPool(websh_server_conf * conf)
{
    Tcl_FindExecutable(NULL);

    DEBUG_TRACE(conf->server, "initPool in thread %ld", Tcl_GetCurrentThread());

    if (conf->mainInterp != NULL || conf->webshPool != NULL) {
	/* we have to cleanup */
#ifndef APACHE2
 	ap_log_printf(conf->server, "initPool: mainInterp or webshPool not NULL");
#else /* APACHE2 */
	ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, 0, conf->server,
		     "initPool: mainInterp or webshPool not NULL");
#endif /* APACHE2 */
	return 0;
    }

    /* create a single main interpreter */
    conf->mainInterp = createMainInterp(conf);

    if (conf->mainInterp == NULL) {
	errno = 0;
#ifndef APACHE2
	ap_log_printf(conf->server, "could'nt create main interp");
#else /* APACHE2 */
	ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, 0, conf->server,
		     "could'nt create main interp");
#endif /* APACHE2 */
	return 0;
    }

    /* create our table of interp classes */
    HashUtlAllocInit(conf->webshPool, TCL_STRING_KEYS);

    /* if we're in threaded mode, spawn a watcher thread
       that runs a possibly defined code and does cleanup, something like:

       if (weAreAThreadedOne) {
       CreateThread(interp);
       }
     */
    /* that's it */

    return 1;
}


/* ----------------------------------------------------------------------------
 * create main interpreter (including init stuff)
 * ------------------------------------------------------------------------- */
Tcl_Interp *createMainInterp(websh_server_conf * conf)
{

    LogPlugIn *logtoap = NULL;
    Tcl_Interp *mainInterp = Tcl_CreateInterp();
    // Tcl_Preserve(mainInterp);
    ApFuncs *apFuncs;

    if (mainInterp == NULL) {
        DEBUG_TRACE(conf->server, "Tcl_CreateInterp mainInterp fail");
	return NULL;
    }

    if (Tcl_InitStubs(mainInterp,"8.2",0) == NULL) {
      DEBUG_TRACE(conf->server, "Tcl_InitStubs mainInterp fail");
      Tcl_DeleteInterp(mainInterp);
      // Tcl_Release(mainInterp);
      return NULL;
    }

    /* just to be sure the memory command is imported if 
       the corresponding Tcl features it */
#ifdef TCL_MEM_DEBUG
    Tcl_InitMemory(mainInterp);
#endif

    apFuncs = createApFuncs();
    if (apFuncs == NULL) {
        Tcl_DeleteInterp(mainInterp);
        // Tcl_Release(mainInterp);
	return NULL;
    }
    Tcl_SetAssocData(mainInterp, WEB_APFUNCS_ASSOC_DATA, destroyApFuncs, (ClientData *) apFuncs);

    /* standard Init */
    if (Tcl_Init(mainInterp) == TCL_ERROR) {
        DEBUG_TRACE(conf->server, "Tcl_Init mainInterp fail with!");
        DEBUG_TRACE(conf->server, Tcl_GetStringResult(mainInterp));
	Tcl_DeleteInterp(mainInterp);
        // Tcl_Release(mainInterp);
	return NULL;
    }

    /* Websh Library Init */
    if (ModWebsh_Init(mainInterp) == TCL_ERROR) {
        DEBUG_TRACE(conf->server, "ModWebsh_Init mainInterp fail!");
	Tcl_DeleteInterp(mainInterp);
        // Tcl_Release(mainInterp);
	return NULL;
    }

    /* --------------------------------------------------------------------------
     * register log handler "apachelog"
     * ----------------------------------------------------------------------- */
    logtoap = createLogPlugIn();
    if (logtoap == NULL) {
	Tcl_DeleteInterp(mainInterp);
        // Tcl_Release(mainInterp);
	return NULL;
    }
    logtoap->constructor = createLogToAp;
    logtoap->destructor = destroyLogToAp;
    logtoap->handler = (int (*)(Tcl_Interp *, ClientData, char *)) logToAp;
    registerLogPlugIn(mainInterp, APCHANNEL, logtoap);

    /* eval init code */
    if (Tcl_Eval(mainInterp, MAININTERP_INITCODE) == TCL_ERROR) {
	Tcl_DeleteInterp(mainInterp);
        // Tcl_Release(mainInterp);
	return NULL;
    }

    Tcl_CreateObjCommand(mainInterp, "web::interpclasscfg",
			 Web_InterpClassCfg, (ClientData) conf, NULL);

    initMainInterp(conf, mainInterp);

    return mainInterp;
}

static int initMainInterp(websh_server_conf * conf, Tcl_Interp *mainInterp)
{
    /* see if we have a config file to evaluate */
    if (conf->scriptName != NULL) {
	if (Tcl_EvalFile(mainInterp, (char *) conf->scriptName) == TCL_ERROR) {
	    errno = 0;
	    AP_LOG_ERROR(conf->server, "%s", Tcl_GetStringResult(mainInterp));
	    return TCL_ERROR;
	}
	Tcl_ResetResult(mainInterp);
    }
    return TCL_OK;
}


/* ----------------------------------------------------------------------------
 * destroy
 * ------------------------------------------------------------------------- */
void destroyPool(websh_server_conf * conf)
{

    if (conf == NULL)
	return;

    DEBUG_TRACE(conf->server, "destroyPool by thread %ld", Tcl_GetCurrentThread());

    if (conf->webshPool != NULL) {
	Tcl_HashEntry *entry;
	Tcl_HashSearch search;

	Tcl_MutexLock(&(conf->webshPoolLock));
	while ((entry = Tcl_FirstHashEntry(conf->webshPool, &search)) != NULL) {
	    /* loop through entries */
	    poolDestroyWebInterpClass((WebInterpClass *) Tcl_GetHashValue(entry));
	    Tcl_DeleteHashEntry(entry);
	}
	Tcl_DeleteHashTable(conf->webshPool);
	Tcl_MutexUnlock(&(conf->webshPoolLock));
	conf->webshPool = NULL;
    }

    if (conf->mainInterp != NULL) {
	/* now delete the interp */
        DEBUG_TRACE(conf->server, "Tcl_DeleteInterp mainInterp by thread %ld", Tcl_GetCurrentThread());
	Tcl_DeleteInterp(conf->mainInterp);
        // Tcl_Release(conf->mainInterp);
	conf->mainInterp = NULL;
    }

}


/* -------------------------------------------------------------------------
 * cleanupPool NOTE: pool must be locked by caller
 * ------------------------------------------------------------------------- */
void cleanupPool(WebshPool *webshPool)
{

    if (webshPool == NULL) return;

    Tcl_HashEntry *entry;
    Tcl_HashSearch search;
    WebInterpClass *webInterpClass;
    WebInterp *webInterp, *expiredInterp;
    time_t t;

    time(&t);

    entry = Tcl_FirstHashEntry(webshPool, &search);
    while (entry != NULL) {
	/* loop through entries */
	webInterpClass = (WebInterpClass *) Tcl_GetHashValue(entry);

	webInterp = webInterpClass->first;
	while (webInterp != NULL) {
	    /* NOTE: check on max requests is done by poolReleaseWebInterp */
	    if ((webInterp->state) == WIP_FREE) {

		/* check for expiry */
		if (webInterpClass->maxidletime
		    && (t - webInterp->lastusedtime) >
		    webInterpClass->maxidletime) {
		    logToAp(webInterp->interp, NULL,
			    "interpreter expired: idle time reached (id %ld, class %s)", webInterp->id, webInterp->interpClass->filename);
		    webInterp->state = WIP_EXPIRED;
		} else {
		    if (webInterpClass->maxttl
			&& (t - webInterp->starttime) >
			webInterpClass->maxttl) {
			logToAp(webInterp->interp, NULL,
				"interpreter expired: time to live reached (id %ld, class %s)", webInterp->id, webInterp->interpClass->filename);
			webInterp->state = WIP_EXPIRED;
		    }
		}
	    }
	    expiredInterp = webInterp;
	    webInterp = webInterp->next;

	    if (expiredInterp->state == WIP_EXPIRED){
		poolDestroyWebInterp(expiredInterp, WIP_CHECK_THREAD);
            }
	}

        // TODO: Need destroy unused WebInterpClass to release resource.
        //       But destroy it too early is not good for performance - can not reuse it.
        //       Rely on destroyPool() to do cleanup may be ok.
        if( 0 && webInterpClass->first == NULL ){
            poolDestroyWebInterpClass(webInterpClass);
	    Tcl_DeleteHashEntry(entry);
            /* XXX: It is inadvisable to modify the structure of the table,
                    e.g. by creating or deleting entries, while the search is in progress,
                    with the exception of deleting the entry returned by Tcl_FirstHashEntry or Tcl_NextHashEntry.
            */
        }
	entry = Tcl_NextHashEntry(&search);
    }

    return;
}


/* ----------------------------------------------------------------------------
 * readWebInterpClassCode
 * ------------------------------------------------------------------------- */
static int readWebInterpClassCode(WebInterp *webInterp, char *filename, long mtime)
{

    WebInterpClass *webInterpClass =  webInterp->interpClass;
    Tcl_Interp *interp = webInterp->interp;
    Tcl_Channel chan;

    chan = Tcl_OpenFileChannel(interp, filename, "r", 0644);
    if (chan == (Tcl_Channel) NULL) {
	Tcl_ResetResult(interp);
	Tcl_AppendResult(interp, "couldn't read file \"", filename,
			 "\": ", Tcl_ErrnoMsg(Tcl_GetErrno()), (char *) NULL);
    } else {
        Tcl_Obj *objPtr = Tcl_NewObj();
	if (Tcl_ReadChars(chan, objPtr, -1, 0) < 0) {
	    Tcl_Close(interp, chan);
	    Tcl_AppendResult(interp, "couldn't read file \"", filename,
			     "\": ", Tcl_ErrnoMsg(Tcl_GetErrno()), (char *) NULL);
	} else {
	    if (Tcl_Close(interp, chan) == TCL_OK) {
		/* finally success ... */
		webInterpClass->code = objPtr;
	        webInterpClass->mtime = mtime;
                Tcl_IncrRefCount(objPtr);
		return TCL_OK;
	    }
	}
        Tcl_DecrRefCount(objPtr);
    }
    return TCL_ERROR;
}

ApFuncs *createApFuncs() {
  ApFuncs *apFuncs = (ApFuncs *) Tcl_Alloc(sizeof(ApFuncs));
  if (apFuncs == NULL)
    return NULL;
  apFuncs->createDefaultResponseObj = createDefaultResponseObj_AP;
  apFuncs->isDefaultResponseObj = isDefaultResponseObj_AP;
  apFuncs->requestGetDefaultChannelName = requestGetDefaultChannelName_AP;
  apFuncs->requestGetDefaultOutChannelName = requestGetDefaultOutChannelName_AP;
  apFuncs->requestFillRequestValues = requestFillRequestValues_AP;
  apFuncs->Web_Initializer = Web_Initializer_AP;
  apFuncs->Web_Finalizer = Web_Finalizer_AP;
  apFuncs->Web_Finalize = Web_Finalize_AP;
  apFuncs->Web_InterpCfg = Web_InterpCfg_AP;
  apFuncs->Web_InterpClassCfg = Web_InterpClassCfg_AP;
  apFuncs->Web_MainEval = Web_MainEval_AP;
  apFuncs->Web_ConfigPath = Web_ConfigPath_AP;
  apFuncs->ModWebsh_Init = ModWebsh_Init_AP;
  return apFuncs;
}

void destroyApFuncs(ClientData apFuncs, Tcl_Interp * interp) {
  if (apFuncs != NULL)
    Tcl_Free((char *) apFuncs);
}

/*
 * webshpool.h - pool of Tcl interpreters for mod_websh
 * nca-073-9
 * 
 * Copyright (c) 1996-2000 by Netcetera AG.
 * Copyright (c) 2001 by Apache Software Foundation.
 * All rights reserved.
 *
 * See the file "license.terms" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * @(#) $Id: interpool.h 777449 2009-05-22 10:13:35Z ronnie $
 *
 */

#ifndef WEBSHPOOL_H
#define WEBSHPOOL_H

#include "tcl.h"		/* tcl is not necesseraly a system-wide include */
#include "macros.h"		/* for WebAssert and friends. can also reside in .c */
#include "hashutl.h"		/* hash table utitilies */
#include <time.h>

#include "modwebsh.h"
#include "mod_websh.h"
#include "modwebsh_cgi.h"

/* ----------------------------------------------------------------------------
 * the interp-pool is kept in a hash table where
 * - key is the ID (calculated from URL)
 * - entry is wInterpList
 * the pool is locked
 * ------------------------------------------------------------------------- */

typedef enum WebInterpState
{
    WIP_INUSE, WIP_FREE, WIP_EXPIRED, WIP_EXPIRED_INUSE
}
WebInterpState;

struct WebInterpClass;


typedef struct WebInterp
{

    request_rec *req;           /* request connection */

    Tcl_Interp *interp;		/* the interp */
    WebInterpState state;	/* its state */

    struct WebInterpClass *interpClass;	/* infos about this type of interp */

    /* code blocks */
    Tcl_Obj *code;		/* per-request code (=file content) */
    Tcl_Obj *dtor;		/* destructor */

    /* dynamic members */

    long numrequests;		/* number of current request */
    long starttime;		/* start time (Tcl does not handle time_t */
    long lastusedtime;		/* last used time */
    long time_request;		/* time when receive request */
    long time_ready;		/* time when interp is ready */
    long id;                    /* id of the interpreter */

    /* we double-link this list so it's easier to remove elements */
    struct WebInterp *next;
    struct WebInterp *prev;

    Tcl_ThreadId originThrdId;	/* Origin thread where this token was created */
}
WebInterp;


typedef Tcl_HashTable WebshPool;

typedef struct WebInterpClass
{

    Tcl_HashTable *webshPool;

    char *filename;

    /* death reasons */
    long maxrequests;
    long maxttl;
    long maxidletime;
    long mtime;
    long nextid;                /* counter for ids of interpreters */

    Tcl_Obj *code;		/* per-request code (=file content) */

    WebInterp *first;
    WebInterp *last;

    /* configuration of our main Interpreter */
    websh_server_conf *conf;

}
WebInterpClass;

/* ----------------------------------------------------------------------------
 * External API
 * ------------------------------------------------------------------------- */

/*
 * creates a new interp or gets it from the pool based on the id
 * if it is new, then the file is loaded and placed into the preq
 * member.
 * also, it initalizes the req/resp structures given the passed
 * apache structures (which need to be type-adapted of course)

 *  this function does the following
    - aquire lock
    - search for id in hash table
    - if not found, set needtocreate = true;
      else: get HashEntry (which is a pooledInterp)
      walk through list (pooledInterp->next) until there is a free one
      if is free, check filemtime, if filemtime< mtime of the file
        - call "unsafeReleaseInterp" of the found interp
      else use this
      if at end of list and none found  -> set neetocreate = true;
    if needtocreate
      create a new Interp
      create a new pooledInterp
      insert it into list (if we have only next, then in the beginning
      of the list)
      if it is a new has entry, create hash entry
    endif
    - set current paramters (numrequests, req and resp structs)
    - release lock
 */

int initPool(websh_server_conf * conf);

WebInterpClass *poolCreateWebInterpClass(websh_server_conf * conf, Tcl_HashTable *webshPool, char *filename, long mtime);

WebInterp *poolGetWebInterp(websh_server_conf * conf, char *filename,
			    long mtime, request_rec * r);
void poolReleaseWebInterp(WebInterp * webInterp);

void destroyPool(websh_server_conf * conf);

void cleanupPool(WebshPool *webshPool);

ApFuncs* createApFuncs();
void destroyApFuncs(ClientData apFuncs, Tcl_Interp *interp);

/* ----------------------------------------------------------------------------
 * Thread specific inter pool
WebInterp *poolGetThreadWebInterp(websh_server_conf *conf, char *filename,
			    long mtime, request_rec * r)
 * ------------------------------------------------------------------------- */

WebInterp *poolGetThreadWebInterp(websh_server_conf *conf, char *filename,
			    long mtime, request_rec * r);
void poolReleaseThreadWebInterp(WebInterp * webInterp);
static apr_status_t destroyPoolThread(void *data);

static inline void expireWebInterp(WebInterp * webInterp){
  if (webInterp->state == WIP_INUSE) {
    webInterp->state = WIP_EXPIRED_INUSE;
  } else if (webInterp->state == WIP_EXPIRED_INUSE) {
    /* keep the state */
  } else {
    webInterp->state = WIP_EXPIRED;
  }
}

#endif

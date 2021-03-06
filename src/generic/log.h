/*
 * log.h --- the logging module of websh3
 * nca-073-9
 * 
 * Copyright (c) 1996-2000 by Netcetera AG.
 * Copyright (c) 2001 by Apache Software Foundation.
 * All rights reserved.
 *
 * See the file "license.terms" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * @(#) $Id: log.h 788707 2009-06-26 14:04:40Z ronnie $
 *
 */

/* ----------------------------------------------------------------------------
 * metaphor -- let me explain the methaphor I am using for the logging module:
 *
 * it looks to me like a snail mail handling process -- you write an
 * address onto your letter and drop it in the box (web::log address
 * letter). The post office decides whether the address makes sense by
 * searching through the list of known addresses, and rejects the
 * letter if it does not pass this filter (manage the list of known
 * addresses with web::loglevel). If the letter is accepted, it goes
 * through the distribution system where there are different bags,
 * labeled with "USA", "Zurich" etc. The distributer will put your
 * letter into the correct bag which then will be handled by the
 * respective post office (manage the list of bags with web::logdest).
 * In our case, once the letter is dropped into one of those bags, it
 * will be delivered right away.
 * ------------------------------------------------------------------------- */

#include "args.h"
#include "hashutl.h"
#include "macros.h"
#include "tcl.h"
#include "webutl.h"
#include "request.h"


#ifndef WEBLOG_H
#define WEBLOG_H

/* --------------------------------------------------------------------------
 * Log levels
 * ------------------------------------------------------------------------*/

#define WEBLOG_DEBUG   "websh.debug"
#define WEBLOG_INFO    "websh.info"
#define WEBLOG_WARNING "websh.warning"
#define WEBLOG_ERROR   "websh.error"


#define WEB_LOG_DEFAULTFORMAT "%x %X [$p] $f.$l: $m\n"

/* --------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
 * SubCommands
 * ------------------------------------------------------------------------- */
#define WEB_LOG_SUBCMD_ADD    "add"
#define WEB_LOG_SUBCMD_DELETE "delete"
#define WEB_LOG_SUBCMD_NAMES  "names"
#define WEB_LOG_SUBCMD_LEVELS "levels"

/* ----------------------------------------------------------------------------
 * Parameters (like "web::cmdurl -cmd aCommand", where there is an argument
 * ------------------------------------------------------------------------- */
#define WEB_LOG_PARAM_FORMAT "-format"
#define WEB_LOG_PARAM_MAXCHAR "-maxchar"

/* --------------------------------------------------------------------------
 * Registered Data
 * ------------------------------------------------------------------------*/
#define WEB_LOG_ASSOC_DATA "web::logData"

/* --------------------------------------------------------------------------
 * messages
 * ------------------------------------------------------------------------*/
#define WEB_LOG_USAGE_LOGDEST_ADD \
  "add ?options? level type ?type-specific-arguments ...?"

/* --------------------------------------------------------------------------
 * other
 * ------------------------------------------------------------------------*/
#define LOG_LIST_INITIAL_SIZE 10
#define LOG_FILTER_PREFIX "loglevel"
#define LOG_DEST_PREFIX "logdest"
#define LOG_SUBSTDEFAULT 0
#define LOG_SAFEDEFAULT 1

/* ----------------------------------------------------------------------------
 * list of possible categories
 * ------------------------------------------------------------------------- */
typedef enum Severity
{
    none, alert, error, warning, info, debug, invalid = -1
}
Severity;

/* ----------------------------------------------------------------------------
 * the log level, as attached to a log message and as used for filtering
 * ------------------------------------------------------------------------- */
typedef struct LogLevel
{
    int keep;
    char *facility;
    Severity minSeverity;
    Severity maxSeverity;	/* only used for filter */
}
LogLevel;

LogLevel *createLogLevel();
int destroyLogLevel(void *level, void *dum);

/* ----------------------------------------------------------------------------
 * plug-in interface
 * ------------------------------------------------------------------------- */
typedef ClientData(LogPlugInConstructor) (Tcl_Interp * interp,
					  ClientData clientData,
					  int objc, Tcl_Obj * CONST objv[]);
typedef int (LogPlugInDestructor) (Tcl_Interp * interp,
				   ClientData clientData);
typedef int (LogPlugInHandler) (Tcl_Interp * interp,
				ClientData clientData, char *msg);

typedef struct LogPlugIn
{
    LogPlugInConstructor *constructor;
    LogPlugInDestructor *destructor;
    LogPlugInHandler *handler;
}
LogPlugIn;
LogPlugIn __declspec(dllexport) *createLogPlugIn();
int destroyLogPlugIn(void *plugIn, void *dum);
int __declspec(dllexport) registerLogPlugIn(Tcl_Interp * interp, char *type, LogPlugIn * plugIn);


/* ----------------------------------------------------------------------------
 * log destination (like stderr, file, channel, ...)
 * ------------------------------------------------------------------------- */
typedef struct LogDest
{
    int keep;
    LogLevel *filter;
    char *format;
    long maxCharInMsg;
    LogPlugIn *plugIn;
    ClientData plugInData;
}
LogDest;
LogDest *createLogDest();
int destroyLogDest(void *dest, void *env);
char *createLogDestName(char *prefix, int cnt);




/* ----------------------------------------------------------------------------
 * log data structure
 * ------------------------------------------------------------------------- */
typedef struct LogData
{
  LogLevel **listOfFilters;
  int filterSize; /* size of filter list */
  LogDest **listOfDests;
  int destSize; /* site of destination list */
  Tcl_HashTable *listOfPlugIns;
  int logSubst;	/* 1: subst log message, 0: don't (default 0) */
  int safeLog;	/* 1: web::log never fails, 0: can throw I/O error (default 1) */
  int keep;  /* flag for log config to keep during initializer code */
  /* needed so that global settings can be accessed */
  RequestData * requestData;
}
LogData;

LogData *createLogData();
void destroyLogData(ClientData clientData, Tcl_Interp * interp);


/* ----------------------------------------------------------------------------
 * Tcl interface and commands
 * ------------------------------------------------------------------------- */
int __declspec(dllexport) log_Init(Tcl_Interp * interp);

int Web_Log(ClientData clientData,
	    Tcl_Interp * interp, int objc, Tcl_Obj * CONST objv[]);

int Web_LogDest(ClientData clientData,
		Tcl_Interp * interp, int objc, Tcl_Obj * CONST objv[]);

int Web_LogFilter(ClientData clientData,
		  Tcl_Interp * interp, int objc, Tcl_Obj * CONST objv[]);


/* ----------------------------------------------------------------------------
 * the functions
 * ------------------------------------------------------------------------- */
char *getSeverityName(Severity aSeverity);
LogLevel *parseLogLevel(Tcl_Interp * interp,
			char *definition, char *defaultfacility, int cnt);
Tcl_Obj *formatMessage(LogLevel * level, char *fmt, long maxCharInMsg,
		       Tcl_Obj * msg);
int doesPass(LogLevel * level, LogLevel * filter);
int doesPassFilters(LogLevel * logLevel, LogLevel ** logLevels, int size);
int logImpl(Tcl_Interp * interp, LogData * logData,
	    char *levelStr, Tcl_Obj * msg);
int webLog(Tcl_Interp * interp, char *levelStr, char *msg);

int sendMsgToDestList(Tcl_Interp * interp,
		       LogData * logData, LogLevel * level, Tcl_Obj * msg);

char * insertIntoDestList(LogData *logData, LogDest *logDest);
char * insertIntoFilterList(LogData *logData, LogLevel *logLevel);

/* ----------------------------------------------------------------------------
 * Logging 
 * ------------------------------------------------------------------------- */
#define WRITE_LOG 1		/* sends message to log facility */
#define SET_RESULT 2		/* store part of message in interpreter result */
#define INTERP_ERRORINFO 4	/* include tcl-interp's errorInfo in log message */

/* default use:
  LOG_MSG(,WRITE_LOG,...)  --> just write log msg
  LOG_MSG(,SET_RESULT,...) --> just set interp result
  LOG_MSG(,WRITE_LOG | SET_RESULT,...) --> log msg and set interp result
  LOG_MSG(,0,...) --> don't do this
*/

void __declspec(dllexport) LOG_MSG(Tcl_Interp * interp, int flag, char *filename, int linenr,
	     char *cmd, char *level, char *msg, ...);

#endif

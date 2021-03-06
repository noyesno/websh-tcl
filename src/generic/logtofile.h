/*
 * logtofile.h -- plugin for log module of websh3
 * nca-073-9
 * 
 * Copyright (c) 1996-2000 by Netcetera AG.
 * Copyright (c) 2001 by Apache Software Foundation.
 * All rights reserved.
 *
 * See the file "license.terms" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * @(#) $Id: logtofile.h 322102 2001-10-25 17:50:15Z davidw $
 *
 */

#ifndef WEB_LOGTOFILE_H
#define WEB_LOGTOFILE_H

/* --------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
 * SubCommands
 * ------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------------
 * Switches (like "string -binary")
 * ------------------------------------------------------------------------- */
#define WEB_LOGTOFILE_SWITCH_UNBUFFERED "-unbuffered"

/* ----------------------------------------------------------------------------
 * Parameters (like "web::cmdurl -cmd aCommand", where there is an argument
 * ------------------------------------------------------------------------- */


/* --------------------------------------------------------------------------
 * Registered Data
 * ------------------------------------------------------------------------*/

/* --------------------------------------------------------------------------
 * messages
 * ------------------------------------------------------------------------*/
#define WEB_LOGTOFILE_USAGE "?-unbuffered? fileName"


/* ----------------------------------------------------------------------------
 * plugin logger: toFile
 * ------------------------------------------------------------------------- */
typedef struct LogToFileData
{
    Tcl_Channel channel;
    char isBuffered;
    char *fileName;
}
LogToFileData;
LogToFileData *createLogToFileData();
int destroyLogToFileData(Tcl_Interp * interp, LogToFileData * logToFileData);

ClientData createLogToFile(Tcl_Interp * interp, ClientData clientData,
			   int objc, Tcl_Obj * CONST objv[]);
int destroyLogToFile(Tcl_Interp * interp, ClientData clientData);
int logToFile(Tcl_Interp * interp, ClientData clientData, char *msg);

#endif

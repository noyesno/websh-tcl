/*
 * weboutint.c --- output handler of websh3
 * nca-073-9
 *
 * Copyright (c) 1996-2000 by Netcetera AG.
 * Copyright (c) 2001 by Apache Software Foundation.
 * All rights reserved.
 *
 * See the file "license.terms" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * @(#) $Id: weboutint.c 572736 2007-09-04 16:59:43Z ronnie $
 *
 */

#include "tcl.h"
#include <stdio.h>
#include "webout.h"		/* is member of output module of websh */
#include "args.h"		/* arg processing */
#include "webutl.h"
#include "hashutl.h"
#include "paramlist.h"		/* destroyParamList */
#include "varchannel.h"

/* ----------------------------------------------------------------------------
 * getChannel
 * ------------------------------------------------------------------------- */
Tcl_Channel getChannel(Tcl_Interp * interp, ResponseObj * responseObj)
{

    int mode = 0;
    Tcl_Channel channel = NULL;

    if ((interp == NULL) || (responseObj == NULL))
	return NULL;

    channel = Web_GetChannelOrVarChannel(interp,
					 Tcl_GetString(responseObj->name),
					 &mode);

    if (channel == NULL) {

	LOG_MSG(interp, WRITE_LOG | SET_RESULT, __FILE__, __LINE__,
		"response", WEBLOG_ERROR,
		"error getting channel \"", Tcl_GetString(responseObj->name),
		"\"", NULL);
	return NULL;
    }

    if (!(mode & TCL_WRITABLE)) {

	LOG_MSG(interp, WRITE_LOG, __FILE__, __LINE__,
		"response", WEBLOG_ERROR,
		"channel \"", Tcl_GetString(responseObj->name),
		"\" not open for writing", NULL);
	return NULL;
    }

    return channel;
}

/* ----------------------------------------------------------------------------
 * getResponseObj
 * ------------------------------------------------------------------------- */
ResponseObj *getResponseObj(Tcl_Interp * interp, OutData * outData,
			    char *name)
{

    ResponseObj *responseObj = NULL;

    if ((interp == NULL) || (outData == NULL))
	return NULL;

    /* --------------------------------------------------------------------------
     * default ?
     * ----------------------------------------------------------------------- */
    if (name == NULL) {

	responseObj = outData->defaultResponseObj;

    } else {

	/* ------------------------------------------------------------------------
	 * do we have it ?
	 * --------------------------------------------------------------------- */
	responseObj =
	    (ResponseObj *) getFromHashTable(outData->responseObjHash, name);
	/* ------------------------------------------------------------------------
	 * no such object. implicitely build one
	 * --------------------------------------------------------------------- */
	if (responseObj == NULL) {

	    int err = 0;
	    responseObj =
		createResponseObj(interp, name, &objectHeaderHandler);

	    if (responseObj == NULL)
		err++;
	    else if (appendToHashTable(outData->responseObjHash,
				       Tcl_GetString(responseObj->name),
				       (ClientData) responseObj) != TCL_OK)
		err++;

	    if (err) {

		LOG_MSG(interp, WRITE_LOG | SET_RESULT, __FILE__, __LINE__,
			"response", WEBLOG_ERROR,
			"error creating response object", NULL);
		return NULL;
	    }
	}
    }

    if (responseObj == NULL) {

	LOG_MSG(interp, WRITE_LOG | SET_RESULT, __FILE__, __LINE__,
		"web::putx", WEBLOG_ERROR,
		"error accessing response object", NULL);
	return NULL;
    }

    return responseObj;
}


/* ----------------------------------------------------------------------------
 * createResponseObj
 * ------------------------------------------------------------------------- */
ResponseObj __declspec(dllexport) *createResponseObj(Tcl_Interp * interp, char *channelName,
			       ResponseHeaderHandler * headerHandler)
{

    Tcl_HashTable *hash = NULL;
    ResponseObj *responseObj;
    char *defheaders[] = { HEADER, NULL };
    int i;

    if (channelName == NULL)
	return NULL;

/*   fprintf(stderr,"creating '%s'\n",channelName); fflush(stderr); */


    /* --------------------------------------------------------------------------
     * create response object
     * ----------------------------------------------------------------------- */
    responseObj = WebAllocInternalData(ResponseObj);

    if (responseObj == NULL) {

	LOG_MSG(interp, WRITE_LOG | SET_RESULT, __FILE__, __LINE__,
		"createResponseObj", WEBLOG_ERROR,
		"error creating internal data", NULL);
	return NULL;
    }

    /* --------------------------------------------------------------------------
     * headers
     * ----------------------------------------------------------------------- */
    HashUtlAllocInit(hash, TCL_STRING_KEYS);

    if (hash == NULL)
	return NULL;

    i = 0;

    while (defheaders[i]) {
	char *key;
	Tcl_Obj *val;

	key = defheaders[i++];
	val = Tcl_NewStringObj(defheaders[i++], -1);

	paramListAdd(hash, key, val);
    }

    responseObj->firstbyte  = 0;
    responseObj->sendHeader = 1;
    responseObj->bytesSent = 0;
    responseObj->headers = hash;
    responseObj->name = Tcl_NewStringObj(channelName, -1);
    responseObj->httpresponse = NULL;
    responseObj->headerHandler = headerHandler;

    Tcl_IncrRefCount(responseObj->name);	/* it's mine */

    return responseObj;
}

/* ----------------------------------------------------------------------------
 * destroyResponseObj
 * ------------------------------------------------------------------------- */
int destroyResponseObj(ClientData clientData, Tcl_Interp * interp)
{

    ResponseObj *responseObj = NULL;

    if (clientData == NULL)
	return TCL_ERROR;

    responseObj = (ResponseObj *) clientData;

    /* unregister if was a varchannel */
/*   printf("DBG destroyResponseObj '%s'\n",Tcl_GetString(responseObj->name)); fflush(stdout); */
    Web_UnregisterVarChannel(interp, Tcl_GetString(responseObj->name), NULL);

    WebDecrRefCountIfNotNull(responseObj->name);
    WebDecrRefCountIfNotNull(responseObj->httpresponse);

    if (responseObj->headers != NULL) {
	destroyParamList(responseObj->headers);
	responseObj->headers = NULL;
    }

    WebFreeIfNotNull(responseObj);

    return TCL_OK;
}

/* ----------------------------------------------------------------------------
 * createResponseObjHash
 * ------------------------------------------------------------------------- */
int createResponseObjHash(OutData * outData)
{

    if (outData == NULL)
	return TCL_ERROR;
    if (outData->defaultResponseObj == NULL)
	return TCL_ERROR;

    HashUtlAllocInit(outData->responseObjHash, TCL_STRING_KEYS);

    if (outData->responseObjHash != NULL) {
	if (appendToHashTable(outData->responseObjHash,
			      Tcl_GetString(outData->defaultResponseObj->
					    name),
			      (ClientData) outData->defaultResponseObj)
	    != TCL_OK) {
	    HashUtlDelFree(outData->responseObjHash);
	    outData->responseObjHash = NULL;
	    return TCL_ERROR;
	}
    }

    return TCL_OK;
}

/* ----------------------------------------------------------------------------
 * destroyResponseObjHash
 * ------------------------------------------------------------------------- */
int destroyResponseObjHash(OutData * outData, Tcl_Interp * interp)
{

    HashTableIterator iterator;
    ResponseObj *responseObj = NULL;

    if (outData == NULL)
	return TCL_ERROR;
    if (outData->responseObjHash == NULL)
	return TCL_ERROR;

    /* delete all response Obj */
    assignIteratorToHashTable(outData->responseObjHash, &iterator);

    while (nextFromHashIterator(&iterator) != TCL_ERROR) {

	responseObj = (ResponseObj *) valueOfCurrent(&iterator);
	if (responseObj != NULL)
	    destroyResponseObj((ClientData) responseObj, interp);
    }

    HashUtlDelFree(outData->responseObjHash);

    outData->responseObjHash = NULL;

    return TCL_OK;
}

/* ----------------------------------------------------------------------------
 * createOutData
 * ------------------------------------------------------------------------- */
OutData *createOutData(Tcl_Interp * interp)
{

    OutData *outData = NULL;

    outData = WebAllocInternalData(OutData);

    if (outData != NULL) {

	outData->defaultResponseObj = createDefaultResponseObj(interp);
	if (outData->defaultResponseObj != NULL) {

	    if (createResponseObjHash(outData) != TCL_OK) {

		destroyResponseObj((ClientData) outData->defaultResponseObj,
				   interp);
		WebFreeIfNotNull(outData);
		return NULL;
	    }
	}
	else {

	    WebFreeIfNotNull(outData);
	    return NULL;
	}

	outData->putxMarkup = PUTXMARKUPDEFAULT;
    }

    return outData;
}

/* ----------------------------------------------------------------------------
 * reset
 * ------------------------------------------------------------------------- */
int resetOutData(Tcl_Interp * interp, OutData * outData)
{

    if ((interp == NULL) || (outData == NULL))
	return TCL_ERROR;

    outData->putxMarkup = PUTXMARKUPDEFAULT;

    if (destroyResponseObjHash(outData, interp) == TCL_ERROR)
	return TCL_ERROR;
    outData->responseObjHash = NULL;

    /* create standard channel */
    outData->defaultResponseObj = createDefaultResponseObj(interp);
    if (outData->defaultResponseObj == NULL)
	return TCL_ERROR;

    /* create Hash (and add default channel) */
    if (createResponseObjHash(outData) != TCL_OK)
	return TCL_ERROR;

    return TCL_OK;
}

/* ----------------------------------------------------------------------------
 * destroy internal data structure
 * ------------------------------------------------------------------------- */
int destroyOutData(ClientData clientData, Tcl_Interp * interp)
{

    OutData *outData;
    /* HashTableIterator iterator; */

    if (clientData == NULL)
	return TCL_ERROR;

    outData = (OutData *) clientData;

    /* delete all response Obj */
    destroyResponseObjHash(outData, interp);

    WebFreeIfNotNull(outData);

    return TCL_OK;
}

/* ----------------------------------------------------------------------------
 * webout_eval_tag (code in <? ?>)
 * ------------------------------------------------------------------------- */
#define PUTX_SUBST_IGNORE    0          // <? set abc 123 ?>
#define PUTX_SUBST_RESULT    1          // <?= set abc 123 ?>
#define PUTX_SUBST_VARIABLE  2          // <?= $abc ?>
int webout_eval_tag(Tcl_Interp * interp, ResponseObj * responseObj,
		    Tcl_Obj * in, TCLCONST char *strstart, TCLCONST char *strend)
{
  Tcl_Obj *outbuf;
  Tcl_Obj *tclo;
  char *next;
  char *cur;

  int endseqlen = strlen(strend);
  int startseqlen = strlen(strstart);
  int begin = 1;
  int firstScan = 1;
  int inside = 0;
  int inLen = 0;
  int res = 0;
  int subst_result = PUTX_SUBST_IGNORE;

  next = Tcl_GetStringFromObj(in, &inLen);
  outbuf = Tcl_NewStringObj("", -1);
  Tcl_IncrRefCount(outbuf);

  if (inLen == 0) {
    Tcl_DecrRefCount(outbuf);
    return 0;
  }


  while (*next != 0) {
    cur = next;
    next = (char *)Tcl_UtfNext(cur);

    if (strncmp("\\", cur, 1) == 0) {
      if (firstScan == 1) { firstScan = 0; }
      if (strncmp(strstart, next, startseqlen) == 0) {
	Tcl_AppendToObj(outbuf, "\\", 1);
	Tcl_AppendToObj(outbuf, strstart, startseqlen);
	next += startseqlen;
      } else if (strncmp(strend, next, endseqlen) == 0) {
	Tcl_AppendToObj(outbuf, "\\", 1);
	Tcl_AppendToObj(outbuf, strend, endseqlen);
	next += endseqlen;
      } else if (inside < 1) {
	Tcl_AppendToObj(outbuf, "\\\\", 2);
      } else {
	Tcl_AppendToObj(outbuf, "\\", 1);
      }
    } else if (strncmp(strstart, cur, startseqlen) == 0) {
      /* see start tag */
      if ((++inside) == 1) {
	if (firstScan == 1) {
	  begin = 0;
	  firstScan = 0;
	  Tcl_AppendToObj(outbuf, "\n", 1);
	} else {
	  Tcl_AppendToObj(outbuf, "\"\n", 2);
	}
	if (startseqlen > 1) {
	  next += startseqlen - 1;
	}

        /* see <?= ... ?> */
        if(next[0] == '='){
          subst_result = PUTX_SUBST_RESULT;
          next++;  // skip '='
          while(next[0]==' ' || next[0]=='\t') next++;
          if(next[0]=='$') {
            subst_result = PUTX_SUBST_VARIABLE;
          }
        } else {
          subst_result = PUTX_SUBST_IGNORE;
        }

        /* TODO: <?= $var ?>  , {{$var}}, [[set var]] */

	switch(subst_result){
	    case PUTX_SUBST_IGNORE:
	      break;
	    case PUTX_SUBST_RESULT:
              /* subst result */
	      Tcl_AppendToObj(outbuf, "web::put [", -1);
	      break;
	    case PUTX_SUBST_VARIABLE:
	      Tcl_AppendToObj(outbuf, "web::put ", -1);
	      break;
        }
      }  else {
	Tcl_AppendToObj(outbuf, cur, startseqlen);
	if (startseqlen > 1) {
	  next += startseqlen - 1;
	}
      }
    } else if (strncmp(strend, cur, endseqlen) == 0) {
      /* see end tag */
      if (firstScan == 1) { firstScan = 0; }
      if ((--inside) == 0) {

        /* subst result */
        if(subst_result==PUTX_SUBST_RESULT){
	  Tcl_AppendToObj(outbuf, "]", -1);
        }else if(subst_result==PUTX_SUBST_VARIABLE){
	  Tcl_AppendToObj(outbuf, "", -1);
        }

	Tcl_AppendToObj(outbuf, "\nweb::put \"", -1);
	if (endseqlen > 1) {
	  next += endseqlen - 1;
	}

      } else {
	Tcl_AppendToObj(outbuf, cur, endseqlen);
	if (endseqlen > 1) {
	  next += endseqlen - 1;
	}
      }
      if (inside < 0) { inside = 0; }
    } else if (inside < 1) {
      if (firstScan == 1) { firstScan = 0; }
      switch (*cur) {
      case '{':
      case '}':
      case '$':
      case '[':
      case ']':
      case '"':
	Tcl_AppendToObj(outbuf, "\\", -1);
      default:
	Tcl_AppendToObj(outbuf, cur, next - cur);
	break;
      }
    } else {
      if (firstScan == 1) { firstScan = 0; }
      Tcl_AppendToObj(outbuf, cur, next - cur);
    }
  }
  if (begin) {
    tclo = Tcl_NewStringObj("web::put \"", -1);
    Tcl_IncrRefCount(tclo); 
    Tcl_AppendObjToObj(tclo, outbuf);
    Tcl_DecrRefCount(outbuf);
  } else {
    tclo = outbuf;
  }
  Tcl_AppendToObj(tclo, "\"", -1);
  res = Tcl_EvalObjEx(interp, tclo, TCL_EVAL_DIRECT);
  Tcl_DecrRefCount(tclo); 
  return res;
}

/* ----------------------------------------------------------------------------
 * putsCmdImpl -- do the work here
 * ------------------------------------------------------------------------- */
int putsCmdImpl(Tcl_Interp * interp, ResponseObj * responseObj, Tcl_Obj * str)
{

    Tcl_Obj *sendString = NULL;
    long bytesSent = 0;
    Tcl_Channel channel;
    Tcl_DString translation;

    /* --------------------------------------------------------------------------
     * sanity
     * ----------------------------------------------------------------------- */
    if ((responseObj == NULL) || (str == NULL))
	return TCL_ERROR;

/*   printf("DBG putsCmdImpl - got '%s'\n",Tcl_GetString(str)); fflush(stdout); */

    channel = getChannel(interp, responseObj);
    if (channel == NULL)
	return TCL_ERROR;

    sendString = Tcl_NewObj();
    Tcl_IncrRefCount(sendString);

    if (responseObj->sendHeader) {
        Tcl_Time tcltime;
        Tcl_GetTime(&tcltime);
        responseObj->firstbyte  = tcltime.sec*1000000 + tcltime.usec;
	responseObj->headerHandler(interp, responseObj, sendString);

    }

    Tcl_AppendObjToObj(sendString, str);

    /* make sure there is no additional newline translation */
    Tcl_DStringInit(&translation);
    Tcl_GetChannelOption(interp, channel, "-translation", &translation);
    Tcl_SetChannelOption(interp, channel, "-translation", "lf");

    if ((bytesSent = Tcl_WriteObj(channel, sendString)) == -1) {

	LOG_MSG(interp, WRITE_LOG | SET_RESULT,
		__FILE__, __LINE__,
		"web::put", WEBLOG_ERROR,
		"error writing to response object:",
		Tcl_GetStringResult(interp), NULL);
	Tcl_DecrRefCount(sendString);
	Tcl_SetChannelOption(interp, channel, "-translation", Tcl_DStringValue(&translation));
	Tcl_DStringFree(&translation);
	return TCL_ERROR;
    }

    Tcl_SetChannelOption(interp, channel, "-translation", Tcl_DStringValue(&translation));
    Tcl_DStringFree(&translation);
    responseObj->bytesSent += bytesSent;

    /* flush varchannel */
    if (responseObj->name != NULL) {
	char *channelName = Tcl_GetString(responseObj->name);
	if (channelName != NULL)
	    if (channelName[0] == '#')
		Tcl_Flush(channel);
    }

    Tcl_DecrRefCount(sendString);
    return TCL_OK;
}

/* ----------------------------------------------------------------------------
 * objectHeaderHandler -- send headers into a Tcl_Obj, used for variables and channels
 * ------------------------------------------------------------------------- */
int objectHeaderHandler(Tcl_Interp * interp, ResponseObj * responseObj,
			Tcl_Obj * out)
{

    /* --------------------------------------------------------------------------
     * sanity
     * ----------------------------------------------------------------------- */
    if ((out == NULL) || (responseObj == NULL))
	return TCL_ERROR;

    if (responseObj->sendHeader == 1) {

	HashTableIterator iterator;
	char *key;
	Tcl_Obj *headerList;

	if (responseObj->httpresponse != NULL) {
	    Tcl_AppendObjToObj(out, responseObj->httpresponse);
	    Tcl_AppendToObj(out, "\r\n", 2);
	}

	assignIteratorToHashTable(responseObj->headers, &iterator);

	while (nextFromHashIterator(&iterator) != TCL_ERROR) {

	    key = keyOfCurrent(&iterator);

	    if (key != NULL) {

		headerList = (Tcl_Obj *) valueOfCurrent(&iterator);
		if (headerList != NULL) {

		    int lobjc = -1;
		    Tcl_Obj **lobjv = NULL;
		    int i;
		    if (Tcl_ListObjGetElements(interp, headerList,
					       &lobjc, &lobjv) == TCL_ERROR) {
			LOG_MSG(interp, WRITE_LOG,
				__FILE__, __LINE__,
				"web::put", WEBLOG_ERROR,
				(char *) Tcl_GetStringResult(interp), NULL);
			return TCL_ERROR;
		    }
		    /* add all occurrences of this header */
		    for (i = 0; i < lobjc; i++) {

			Tcl_AppendToObj(out, key, -1);
			Tcl_AppendToObj(out, ": ", 2);
			Tcl_AppendObjToObj(out, lobjv[i]);
			Tcl_AppendToObj(out, "\r\n", 2);
		    }
		}
	    }
	}
	Tcl_AppendToObj(out, "\r\n", 2);
	responseObj->sendHeader = 0;
    }
    return TCL_OK;
}

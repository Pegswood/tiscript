/* parse.c - argument parsing function */
/*
        Copyright (c) 2001-2004 Terra Informatica Software, Inc.
        and Andrew Fedoniouk andrew@terrainformatica.com
        All rights reserved
*/

#include "cs.h"

namespace tis 
{

/*
value CsGetNthArg(VM *c, int n)
{
  return CsGetArg(c,n);
}

int CsGetArgCount(VM *c)
{
  return CsArgCnt(c);
}*/


/* CsParseArguments - parse the argument list of a method */
int CsParseArguments(VM *c,char *fmt,...)
{
    int spec,optionalP;
    value *argv = c->argv;
    int argc = c->argc;
    value arg;
    va_list ap;

    /* get the variable argument list */
    va_start(ap,fmt);

    /* no optional specifier seen yet */
    optionalP = FALSE;

    /* handle each argument specifier */
    while (*fmt) {

        /* check for the optional specifier */
        if ((spec = *fmt++) == '|')
            optionalP = true;

        /* handle argument specifiers */
        else {

            /* check for another argument */
            if (--argc < 0)
				        break;

            /* get the argument */
            arg = *--argv;

            /* pdispatch on the specifier */
            switch (spec) 
            {
            case '*':   /* skip */
                break;
            case 'c':   /* char */
                {   char *p = va_arg(ap,char *);
                    if (!CsIntegerP(arg))
                        //CsTypeError(c,arg);
                        CsUnexpectedTypeError(c,arg,"integer");
                    *p = (char)CsIntegerValue(arg);
                }
                break;
            case 's':   /* short */
                {   short *p = va_arg(ap,short *);
                    if (!CsIntegerP(arg))
                        CsTypeError(c,arg);
                    *p = (short)CsIntegerValue(arg);
                }
                break;
            case 'i':   /* int */
                {   int *p = va_arg(ap,int *);
                    if (!CsIntegerP(arg))
                        //CsTypeError(c,arg);
                        CsUnexpectedTypeError(c,arg,"integer");
                    *p = (int)CsIntegerValue(arg);
                }
                break;
            case 'l':   /* long */
                {   long *p = va_arg(ap,long *);
                    if (!CsIntegerP(arg))
                        //CsTypeError(c,arg);
                        CsUnexpectedTypeError(c,arg,"integer");
                    *p = (long)CsIntegerValue(arg);
                }
                break;
            case 'f':   /* float */
                {   float *p = va_arg(ap,float *);
                    if (CsIntegerP(arg))
                      *p = (float)CsIntegerValue(arg);
                    else if (!CsFloatP(arg))
                      //CsTypeError(c,arg);
                      CsUnexpectedTypeError(c,arg,"float");
                    else
                      *p = (float)CsFloatValue(arg);
                }
                break;
            case 'd':   /* double */
                {   double *p = va_arg(ap,double *);
                    if (CsIntegerP(arg))
                      *p = (double)CsIntegerValue(arg);
                    else if (CsFloatP(arg))
                      *p = (double)CsFloatValue(arg);
                    else
                      //CsTypeError(c,arg);
                      CsUnexpectedTypeError(c,arg,"float");
                }
                break;
            case 'S':   /* string */
                {   
                    wchar **p = va_arg(ap,wchar **);
                    int nilAllowedP = FALSE;
                    if (*fmt == '?') {
                        nilAllowedP = true;
                        ++fmt;
                    }
                    if (nilAllowedP && arg == c->undefinedValue)
                        *p = NULL;
                    else if (CsStringP(arg))
                        *p = CsStringAddress(arg);
                    else
                        CsTypeError(c,arg);
                    if (*fmt == '#') {
                        int *p = va_arg(ap,int *);
                        *p = CsStringSize(arg);
                        ++fmt;
                    }
                }
                break;
            case 'V':   /* value */
                {   
                    value *p = va_arg(ap,value *);
                    int nilAllowedP = FALSE;
                    if (*fmt == '?') {
                        nilAllowedP = true;
                        ++fmt;
                    }
                    if (nilAllowedP && arg == c->undefinedValue)
                        *p = 0;
                    else {
                        if (*fmt == '=') {
                            dispatch *desiredType = va_arg(ap,dispatch *);
                            dispatch *type = CsGetDispatch(arg);
                            for (;;) {
                                if (type == desiredType || type->baseType == desiredType)
                                    break;
                                else if (!(type = type->proto))
                                    CsUnexpectedTypeError(c,arg,desiredType->typeName);
                            }
                            ++fmt;
                        }
                        *p = arg;
                    }
                }
                break;
            case 'P':   /* foreign pointer */
                {   void **p = va_arg(ap,void **);
                    int nilAllowedP = FALSE;
                    if (*fmt == '?') {
                        nilAllowedP = true;
                        ++fmt;
                    }
                    if (nilAllowedP && arg == c->undefinedValue)
                        *p = NULL;
                    else {
                        if (*fmt == '=') {
                            dispatch *desiredType = va_arg(ap,dispatch *);
                            dispatch *type = CsGetDispatch(arg);
                            for (;;) {
                                if (type->baseType == desiredType)
                                    break;
                                else if (!(type = type->proto))
                                    //CsTypeError(c,arg);
                                    CsUnexpectedTypeError(c,arg,desiredType->typeName);
                            }
                            ++fmt;
                        }
                        *p = CsCObjectValue(arg);
                    }
                }
                break;
            case 'B':   /* boolean */
                {   bool *p = va_arg(ap,bool *);
                    *p = CsTrueP(c,arg);
                }
                break;
            case 'L':  /* symbol */
                {
                    symbol_t *p = va_arg(ap,symbol_t *);
                    if (!CsSymbolP(arg))
                          CsUnexpectedTypeError(c,arg,"symbol");
                    *p = (int)to_symbol(arg);
                }
                break;
            default:
                CsThrowKnownError(c,CsErrBadParseCode,(void *)spec);
                break;
            }
        }
    }

    /* finished with the variable arguments */
    va_end(ap);

    /* check for too many arguments */

    //if (argc > 0)
    //    CsTooManyArguments(c);
    //else 
    if (argc < 0 && !optionalP)
	      CsTooFewArguments(c);

    return argc;

}

}

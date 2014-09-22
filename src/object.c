/* Object implementation.
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "prez.h"
#include <math.h>
#include <ctype.h>

#ifdef __CYGWIN__
#define strtold(a,b) ((long double)strtod((a),(b)))
#endif

robj *createObject(int type, void *ptr) {
    robj *o = zmalloc(sizeof(*o));
    o->type = type;
    o->encoding = PREZ_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;

    /* Set the LRU to the current lruclock (minutes resolution). */
    o->lru = LRU_CLOCK();
    return o;
}

/* Create a string object with encoding PREZ_ENCODING_RAW, that is a plain
 * string object where o->ptr points to a proper sds string. */
robj *createRawStringObject(char *ptr, size_t len) {
    return createObject(PREZ_STRING,sdsnewlen(ptr,len));
}

/* Create a string object with encoding PREZ_ENCODING_EMBSTR, that is
 * an object where the sds string is actually an unmodifiable string
 * allocated in the same chunk as the object itself. */
robj *createEmbeddedStringObject(char *ptr, size_t len) {
    robj *o = zmalloc(sizeof(robj)+sizeof(struct sdshdr)+len+1);
    struct sdshdr *sh = (void*)(o+1);

    o->type = PREZ_STRING;
    o->encoding = PREZ_ENCODING_EMBSTR;
    o->ptr = sh+1;
    o->refcount = 1;
    o->lru = LRU_CLOCK();

    sh->len = len;
    sh->free = 0;
    if (ptr) {
        memcpy(sh->buf,ptr,len);
        sh->buf[len] = '\0';
    } else {
        memset(sh->buf,0,len+1);
    }
    return o;
}

/* Create a string object with EMBSTR encoding if it is smaller than
 * REIDS_ENCODING_EMBSTR_SIZE_LIMIT, otherwise the RAW encoding is
 * used.
 *
 * The current limit of 39 is chosen so that the biggest string object
 * we allocate as EMBSTR will still fit into the 64 byte arena of jemalloc. */
#define PREZ_ENCODING_EMBSTR_SIZE_LIMIT 39
robj *createStringObject(char *ptr, size_t len) {
    if (len <= PREZ_ENCODING_EMBSTR_SIZE_LIMIT)
        return createEmbeddedStringObject(ptr,len);
    else
        return createRawStringObject(ptr,len);
}

robj *createStringObjectFromLongLong(long long value) {
    robj *o;
    if (value >= 0 && value < PREZ_SHARED_INTEGERS) {
        incrRefCount(shared.integers[value]);
        o = shared.integers[value];
    } else {
        if (value >= LONG_MIN && value <= LONG_MAX) {
            o = createObject(PREZ_STRING, NULL);
            o->encoding = PREZ_ENCODING_INT;
            o->ptr = (void*)((long)value);
        } else {
            o = createObject(PREZ_STRING,sdsfromlonglong(value));
        }
    }
    return o;
}

/* Note: this function is defined into object.c since here it is where it
 * belongs but it is actually designed to be used just for INCRBYFLOAT */
robj *createStringObjectFromLongDouble(long double value) {
    char buf[256];
    int len;

    /* We use 17 digits precision since with 128 bit floats that precision
     * after rounding is able to represent most small decimal numbers in a way
     * that is "non surprising" for the user (that is, most small decimal
     * numbers will be represented in a way that when converted back into
     * a string are exactly the same as what the user typed.) */
    len = snprintf(buf,sizeof(buf),"%.17Lf", value);
    /* Now remove trailing zeroes after the '.' */
    if (strchr(buf,'.') != NULL) {
        char *p = buf+len-1;
        while(*p == '0') {
            p--;
            len--;
        }
        if (*p == '.') len--;
    }
    return createStringObject(buf,len);
}

/* Duplicate a string object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * This function also guarantees that duplicating a small integere object
 * (or a string object that contains a representation of a small integer)
 * will always result in a fresh object that is unshared (refcount == 1).
 *
 * The resulting object always has refcount set to 1. */
robj *dupStringObject(robj *o) {
    robj *d;

    prezAssert(o->type == PREZ_STRING);

    switch(o->encoding) {
    case PREZ_ENCODING_RAW:
        return createRawStringObject(o->ptr,sdslen(o->ptr));
    case PREZ_ENCODING_EMBSTR:
        return createEmbeddedStringObject(o->ptr,sdslen(o->ptr));
    case PREZ_ENCODING_INT:
        d = createObject(PREZ_STRING, NULL);
        d->encoding = PREZ_ENCODING_INT;
        d->ptr = o->ptr;
        return d;
    default:
        prezPanic("Wrong encoding.");
        break;
    }
}

robj *createListObject(void) {
    list *l = listCreate();
    robj *o = createObject(PREZ_LIST,l);
    listSetFreeMethod(l,decrRefCountVoid);
    o->encoding = PREZ_ENCODING_LINKEDLIST;
    return o;
}

#if 0
robj *createZiplistObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(PREZ_LIST,zl);
    o->encoding = PREZ_ENCODING_ZIPLIST;
    return o;
}

robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType,NULL);
    robj *o = createObject(PREZ_SET,d);
    o->encoding = PREZ_ENCODING_HT;
    return o;
}

robj *createIntsetObject(void) {
    intset *is = intsetNew();
    robj *o = createObject(PREZ_SET,is);
    o->encoding = PREZ_ENCODING_INTSET;
    return o;
}

robj *createHashObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(PREZ_HASH, zl);
    o->encoding = PREZ_ENCODING_ZIPLIST;
    return o;
}

robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));
    robj *o;

    zs->dict = dictCreate(&zsetDictType,NULL);
    zs->zsl = zslCreate();
    o = createObject(PREZ_ZSET,zs);
    o->encoding = PREZ_ENCODING_SKIPLIST;
    return o;
}

robj *createZsetZiplistObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(PREZ_ZSET,zl);
    o->encoding = PREZ_ENCODING_ZIPLIST;
    return o;
}
#endif

void freeStringObject(robj *o) {
    if (o->encoding == PREZ_ENCODING_RAW) {
        sdsfree(o->ptr);
    }
}

void freeListObject(robj *o) {
    switch (o->encoding) {
    case PREZ_ENCODING_LINKEDLIST:
        listRelease((list*) o->ptr);
        break;
    case PREZ_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        prezPanic("Unknown list encoding type");
    }
}
#if 0
void freeSetObject(robj *o) {
    switch (o->encoding) {
    case PREZ_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    case PREZ_ENCODING_INTSET:
        zfree(o->ptr);
        break;
    default:
        prezPanic("Unknown set encoding type");
    }
}

void freeZsetObject(robj *o) {
    zset *zs;
    switch (o->encoding) {
    case PREZ_ENCODING_SKIPLIST:
        zs = o->ptr;
        dictRelease(zs->dict);
        zslFree(zs->zsl);
        zfree(zs);
        break;
    case PREZ_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        prezPanic("Unknown sorted set encoding");
    }
}

void freeHashObject(robj *o) {
    switch (o->encoding) {
    case PREZ_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    case PREZ_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        prezPanic("Unknown hash encoding type");
        break;
    }
}
#endif

void incrRefCount(robj *o) {
    o->refcount++;
}

void decrRefCount(robj *o) {
    if (o->refcount <= 0) prezPanic("decrRefCount against refcount <= 0");
    if (o->refcount == 1) {
        switch(o->type) {
        case PREZ_STRING: freeStringObject(o); break;
        case PREZ_LIST: freeListObject(o); break;
#if 0
        case PREZ_SET: freeSetObject(o); break;
        case PREZ_ZSET: freeZsetObject(o); break;
        case PREZ_HASH: freeHashObject(o); break;
#endif
        default: prezPanic("Unknown object type"); break;
        }
        zfree(o);
    } else {
        o->refcount--;
    }
}

/* This variant of decrRefCount() gets its argument as void, and is useful
 * as free method in data structures that expect a 'void free_object(void*)'
 * prototype for the free method. */
void decrRefCountVoid(void *o) {
    decrRefCount(o);
}

/* This function set the ref count to zero without freeing the object.
 * It is useful in order to pass a new object to functions incrementing
 * the ref count of the received object. Example:
 *
 *    functionThatWillIncrementRefCount(resetRefCount(CreateObject(...)));
 *
 * Otherwise you need to resort to the less elegant pattern:
 *
 *    *obj = createObject(...);
 *    functionThatWillIncrementRefCount(obj);
 *    decrRefCount(obj);
 */
robj *resetRefCount(robj *obj) {
    obj->refcount = 0;
    return obj;
}

int checkType(prezClient *c, robj *o, int type) {
    if (o->type != type) {
        addReply(c,shared.wrongtypeerr);
        return 1;
    }
    return 0;
}

int isObjectRepresentableAsLongLong(robj *o, long long *llval) {
    prezAssertWithInfo(NULL,o,o->type == PREZ_STRING);
    if (o->encoding == PREZ_ENCODING_INT) {
        if (llval) *llval = (long) o->ptr;
        return PREZ_OK;
    } else {
        return string2ll(o->ptr,sdslen(o->ptr),llval) ? PREZ_OK : PREZ_ERR;
    }
}

/* Try to encode a string object in order to save space */
robj *tryObjectEncoding(robj *o) {
    //long value;
    sds s = o->ptr;
    size_t len;

    /* Make sure this is a string object, the only type we encode
     * in this function. Other types use encoded memory efficient
     * representations but are handled by the commands implementing
     * the type. */
    prezAssertWithInfo(NULL,o,o->type == PREZ_STRING);

    /* We try some specialized encoding only for objects that are
     * RAW or EMBSTR encoded, in other words objects that are still
     * in represented by an actually array of chars. */
    if (!sdsEncodedObject(o)) return o;

    /* It's not safe to encode shared objects: shared objects can be shared
     * everywhere in the "object space" of Prez and may end in places where
     * they are not handled. We handle them only as values in the keyspace. */
     if (o->refcount > 1) return o;

    /* Check if we can represent this string as a long integer.
     * Note that we are sure that a string larger than 21 chars is not
     * representable as a 32 nor 64 bit integer. */
    len = sdslen(s);
#if 0
    if (len <= 21 && string2l(s,len,&value)) {
        /* This object is encodable as a long. Try to use a shared object.
         * Note that we avoid using shared integers when maxmemory is used
         * because every object needs to have a private LRU field for the LRU
         * algorithm to work well. */
        if ((server.maxmemory == 0 ||
             (server.maxmemory_policy != PREZ_MAXMEMORY_VOLATILE_LRU &&
              server.maxmemory_policy != PREZ_MAXMEMORY_ALLKEYS_LRU)) &&
            value >= 0 &&
            value < PREZ_SHARED_INTEGERS)
        {
            decrRefCount(o);
            incrRefCount(shared.integers[value]);
            return shared.integers[value];
        } else {
            if (o->encoding == PREZ_ENCODING_RAW) sdsfree(o->ptr);
            o->encoding = PREZ_ENCODING_INT;
            o->ptr = (void*) value;
            return o;
        }
    }
#endif
    /* If the string is small and is still RAW encoded,
     * try the EMBSTR encoding which is more efficient.
     * In this representation the object and the SDS string are allocated
     * in the same chunk of memory to save space and cache misses. */
    if (len <= PREZ_ENCODING_EMBSTR_SIZE_LIMIT) {
        robj *emb;

        if (o->encoding == PREZ_ENCODING_EMBSTR) return o;
        emb = createEmbeddedStringObject(s,sdslen(s));
        decrRefCount(o);
        return emb;
    }

    /* We can't encode the object...
     *
     * Do the last try, and at least optimize the SDS string inside
     * the string object to require little space, in case there
     * is more than 10% of free space at the end of the SDS string.
     *
     * We do that only for relatively large strings as this branch
     * is only entered if the length of the string is greater than
     * PREZ_ENCODING_EMBSTR_SIZE_LIMIT. */
    if (o->encoding == PREZ_ENCODING_RAW &&
        sdsavail(s) > len/10)
    {
        o->ptr = sdsRemoveFreeSpace(o->ptr);
    }

    /* Return the original object. */
    return o;
}

/* Get a decoded version of an encoded object (returned as a new object).
 * If the object is already raw-encoded just increment the ref count. */
robj *getDecodedObject(robj *o) {
    robj *dec;

    if (sdsEncodedObject(o)) {
        incrRefCount(o);
        return o;
    }
    if (o->type == PREZ_STRING && o->encoding == PREZ_ENCODING_INT) {
        char buf[32];

        ll2string(buf,32,(long)o->ptr);
        dec = createStringObject(buf,strlen(buf));
        return dec;
    } else {
        prezPanic("Unknown encoding type");
    }
}

/* Compare two string objects via strcmp() or strcoll() depending on flags.
 * Note that the objects may be integer-encoded. In such a case we
 * use ll2string() to get a string representation of the numbers on the stack
 * and compare the strings, it's much faster than calling getDecodedObject().
 *
 * Important note: when PREZ_COMPARE_BINARY is used a binary-safe comparison
 * is used. */

#define PREZ_COMPARE_BINARY (1<<0)
#define PREZ_COMPARE_COLL (1<<1)

int compareStringObjectsWithFlags(robj *a, robj *b, int flags) {
    prezAssertWithInfo(NULL,a,a->type == PREZ_STRING && b->type == PREZ_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    size_t alen, blen, minlen;

    if (a == b) return 0;
    if (sdsEncodedObject(a)) {
        astr = a->ptr;
        alen = sdslen(astr);
    } else {
        alen = ll2string(bufa,sizeof(bufa),(long) a->ptr);
        astr = bufa;
    }
    if (sdsEncodedObject(b)) {
        bstr = b->ptr;
        blen = sdslen(bstr);
    } else {
        blen = ll2string(bufb,sizeof(bufb),(long) b->ptr);
        bstr = bufb;
    }
    if (flags & PREZ_COMPARE_COLL) {
        return strcoll(astr,bstr);
    } else {
        int cmp;

        minlen = (alen < blen) ? alen : blen;
        cmp = memcmp(astr,bstr,minlen);
        if (cmp == 0) return alen-blen;
        return cmp;
    }
}

/* Wrapper for compareStringObjectsWithFlags() using binary comparison. */
int compareStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,PREZ_COMPARE_BINARY);
}

/* Wrapper for compareStringObjectsWithFlags() using collation. */
int collateStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,PREZ_COMPARE_COLL);
}

/* Equal string objects return 1 if the two objects are the same from the
 * point of view of a string comparison, otherwise 0 is returned. Note that
 * this function is faster then checking for (compareStringObject(a,b) == 0)
 * because it can perform some more optimization. */
int equalStringObjects(robj *a, robj *b) {
    if (a->encoding == PREZ_ENCODING_INT &&
        b->encoding == PREZ_ENCODING_INT){
        /* If both strings are integer encoded just check if the stored
         * long is the same. */
        return a->ptr == b->ptr;
    } else {
        return compareStringObjects(a,b) == 0;
    }
}

size_t stringObjectLen(robj *o) {
    prezAssertWithInfo(NULL,o,o->type == PREZ_STRING);
    if (sdsEncodedObject(o)) {
        return sdslen(o->ptr);
    } else {
        char buf[32];

        return ll2string(buf,32,(long)o->ptr);
    }
}

int getDoubleFromObject(robj *o, double *target) {
    double value;
    char *eptr;

    if (o == NULL) {
        value = 0;
    } else {
        prezAssertWithInfo(NULL,o,o->type == PREZ_STRING);
        if (sdsEncodedObject(o)) {
            errno = 0;
            value = strtod(o->ptr, &eptr);
            if (isspace(((char*)o->ptr)[0]) ||
                eptr[0] != '\0' ||
                (errno == ERANGE &&
                    (value == HUGE_VAL || value == -HUGE_VAL || value == 0)) ||
                errno == EINVAL ||
                isnan(value))
                return PREZ_ERR;
        } else if (o->encoding == PREZ_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            prezPanic("Unknown string encoding");
        }
    }
    *target = value;
    return PREZ_OK;
}

int getDoubleFromObjectOrReply(prezClient *c, robj *o, double *target, const char *msg) {
    double value;
    if (getDoubleFromObject(o, &value) != PREZ_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not a valid float");
        }
        return PREZ_ERR;
    }
    *target = value;
    return PREZ_OK;
}

int getLongDoubleFromObject(robj *o, long double *target) {
    long double value;
    char *eptr;

    if (o == NULL) {
        value = 0;
    } else {
        prezAssertWithInfo(NULL,o,o->type == PREZ_STRING);
        if (sdsEncodedObject(o)) {
            errno = 0;
            value = strtold(o->ptr, &eptr);
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' ||
                errno == ERANGE || isnan(value))
                return PREZ_ERR;
        } else if (o->encoding == PREZ_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            prezPanic("Unknown string encoding");
        }
    }
    *target = value;
    return PREZ_OK;
}

int getLongDoubleFromObjectOrReply(prezClient *c, robj *o, long double *target, const char *msg) {
    long double value;
    if (getLongDoubleFromObject(o, &value) != PREZ_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not a valid float");
        }
        return PREZ_ERR;
    }
    *target = value;
    return PREZ_OK;
}

int getLongLongFromObject(robj *o, long long *target) {
    long long value;
    char *eptr;

    if (o == NULL) {
        value = 0;
    } else {
        prezAssertWithInfo(NULL,o,o->type == PREZ_STRING);
        if (sdsEncodedObject(o)) {
            errno = 0;
            value = strtoll(o->ptr, &eptr, 10);
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' ||
                errno == ERANGE)
                return PREZ_ERR;
        } else if (o->encoding == PREZ_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            prezPanic("Unknown string encoding");
        }
    }
    if (target) *target = value;
    return PREZ_OK;
}

int getLongLongFromObjectOrReply(prezClient *c, robj *o, long long *target, const char *msg) {
    long long value;
    if (getLongLongFromObject(o, &value) != PREZ_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not an integer or out of range");
        }
        return PREZ_ERR;
    }
    *target = value;
    return PREZ_OK;
}

int getLongFromObjectOrReply(prezClient *c, robj *o, long *target, const char *msg) {
    long long value;

    if (getLongLongFromObjectOrReply(c, o, &value, msg) != PREZ_OK) return PREZ_ERR;
    if (value < LONG_MIN || value > LONG_MAX) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is out of range");
        }
        return PREZ_ERR;
    }
    *target = value;
    return PREZ_OK;
}

char *strEncoding(int encoding) {
    switch(encoding) {
    case PREZ_ENCODING_RAW: return "raw";
    case PREZ_ENCODING_INT: return "int";
    case PREZ_ENCODING_HT: return "hashtable";
    case PREZ_ENCODING_LINKEDLIST: return "linkedlist";
    case PREZ_ENCODING_ZIPLIST: return "ziplist";
    case PREZ_ENCODING_INTSET: return "intset";
    case PREZ_ENCODING_SKIPLIST: return "skiplist";
    case PREZ_ENCODING_EMBSTR: return "embstr";
    default: return "unknown";
    }
}
#if 0
/* Given an object returns the min number of milliseconds the object was never
 * requested, using an approximated LRU algorithm. */
unsigned long long estimateObjectIdleTime(robj *o) {
    unsigned long long lruclock = LRU_CLOCK();
    if (lruclock >= o->lru) {
        return (lruclock - o->lru) * PREZ_LRU_CLOCK_RESOLUTION;
    } else {
        return (lruclock + (PREZ_LRU_CLOCK_MAX - o->lru)) *
                    PREZ_LRU_CLOCK_RESOLUTION;
    }
}

/* This is a helper function for the OBJECT command. We need to lookup keys
 * without any modification of LRU or other parameters. */
robj *objectCommandLookup(prezClient *c, robj *key) {
    dictEntry *de;

    if ((de = dictFind(c->db->dict,key->ptr)) == NULL) return NULL;
    return (robj*) dictGetVal(de);
}

robj *objectCommandLookupOrReply(prezClient *c, robj *key, robj *reply) {
    robj *o = objectCommandLookup(c,key);

    if (!o) addReply(c, reply);
    return o;
}

/* Object command allows to inspect the internals of an Prez Object.
 * Usage: OBJECT <refcount|encoding|idletime> <key> */
void objectCommand(prezClient *c) {
    robj *o;

    if (!strcasecmp(c->argv[1]->ptr,"refcount") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyLongLong(c,o->refcount);
    } else if (!strcasecmp(c->argv[1]->ptr,"encoding") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyBulkCString(c,strEncoding(o->encoding));
    } else if (!strcasecmp(c->argv[1]->ptr,"idletime") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyLongLong(c,estimateObjectIdleTime(o)/1000);
    } else {
        addReplyError(c,"Syntax error. Try OBJECT (refcount|encoding|idletime)");
    }
}
#endif

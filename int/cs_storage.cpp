/*
        Copyright (c) 2006 Alexei Marinets marinets@gmail.com
        All rights reserved
*/

#include "cs.h"
#include "dybase.h"
#include "tl_value.h"
#include "tl_hash.h"

#ifdef _DEBUG
#define trace   c->standardOutput->printf
#else
#define trace
#endif

//#pragma optimize( "", off )

namespace tis
{

char* strErrCantSaveObj = "Can't save object.";
char* strErrCorruptPersistent = "Object is corrupted.";

/* name of a property for registered class name */
static const char* _class = "_class";


/* 'storage' pdispatch */

/* method handlers */
static value CSF_open(VM *c);
static value CSF_close(VM *c);
static value CSF_commit(VM *c);
static value CSF_rollback(VM *c);
static value CSF_removeObject(VM *c);
static value CSF_createIndex(VM *c);
static value CSF_registerClass(VM *c);
static value CSF_get_root(VM *c, value obj);
static void  CSF_set_root(VM *c, value obj, value val);
static value CSF_get_autocommit(VM *c, value obj);

/* Storage methods */
static c_method methods[] = {
//	C_METHOD_ENTRY( "this",      CSF_ctor            ),
C_METHOD_ENTRY( "open",		          CSF_open			      ),
C_METHOD_ENTRY( "close",            CSF_close           ),
C_METHOD_ENTRY( "commit",			      CSF_commit			    ),
C_METHOD_ENTRY( "rollback",			    CSF_rollback		    ),

C_METHOD_ENTRY( "deallocateObject", CSF_removeObject),
C_METHOD_ENTRY( "createIndex",	    CSF_createIndex	    ),

/* bonus features */
//C_METHOD_ENTRY( "registerClass",    CSF_registerClass   ),

C_METHOD_ENTRY(	0,                  0                   )
};

/* storage properties */
static vp_method properties[] = {
VP_METHOD_ENTRY( "root",		    CSF_get_root,		      CSF_set_root	      ),
VP_METHOD_ENTRY( "autocommit",  CSF_get_autocommit,   0                   ),
VP_METHOD_ENTRY( 0,             0,					          0				            )
};


#define dybase_date_type      dybase_long_type

/* prototypes */
class storage;
/* returns true if obj is persistent */
/* persistent objects:
    - object
    - vector
    - dbIndex
*/

//typedef dybase_storage_t storage;
void DestroyStorage(VM *c,value obj);


static value FetchValue( VM *c, value vs, dybase_handle_t h );
static void  StoreValue( VM *c, value vs, dybase_handle_t h, value v );
tool::string FetchClassName(storage* s, oid_t oid);

/* --- external interfaces --- */
/* check for initialized persistent object,
i.e. there is a storage assosiated with this obj */
bool CsIsPersistInit(VM *c, value obj)
{ return (CsPersistentP(c, obj) && ptr<persistent_header>(obj)->vstorage); }


/* restore persistent obj from an assosiated storage */
value CsRestoreObj( VM *c, value obj )
{
  if( !CsIsPersistInit(c, obj) )
  { return c->falseValue; }

  value vs = ptr<persistent_header>(obj)->vstorage;
  assert( vs );
  //return ReloadObj( c, vs, obj );
  CsThrowKnownError(c, CsErrPersistError, "'restore' method is not implemented yet");
//#pragma TODO("do reload!")
  return c->falseValue;
}

/* storage */
storage::~storage()
{
	DetachAllObjs(0);
	if( dbS )
	{ dybase_close( dbS ); }

  hashNameProto.clear();
}

	// remove obj from hash
void storage::DetachObj( dybase_oid_t oid )
{
	this->resetPersistHdr( this->hashS[oid] );
	this->hashS.remove(oid);
}


void storage::CommitHash(VM* c)
{
  if(!this->hashS.size())
    return;

  // make snapshot of the hash, this is needed 
  // because this->hashS can be changed (indirectly) by CsStoreObjectData 
  // and CsStoreVectorData functions

	tool::array<value> objs = this->hashS.elements();
  if(c)
	  for( int_t n = objs.last_index(); n >= 0 ; --n)
	  {
      value obj = objs[n];
      if( CsObjectP(obj) )
        CsStoreObjectData(c,obj);
      else if( CsVectorP(obj) )
        CsStoreVectorData(c,obj);
      else if( CsMovedVectorP(obj) )
        CsStoreVectorData(c,obj);

      //else if(  )
      //  CsStoreVectorData(c,CsVectorForwardingAddr(obj));
      else if( CsDbIndexP(c, obj) )
        continue; //?
    }
}

void storage::DetachAllObjs(VM* c)
{
  if(!this->hashS.size())
    return;

  CommitHash(c);

	tool::array<value>& objs = this->hashS.elements();

	for( int_t i = 0; i < objs.size(); i++)
    resetPersistHdr( objs[i] );

  hashS.clear();
}

// set persistent obj attributes to 0:0 (oid:storage)
void storage::resetPersistHdr( value& obj )
{
	ptr<persistent_header>(obj)->oid = 0;
	ptr<persistent_header>(obj)->vstorage = 0;
	ptr<persistent_header>(obj)->status = 0;
}


/* return object from storage hash or '0' if not present */
value storage::GetFromHash( oid_t oid )
{
  value v = 0; this->hashS.find(oid,v); return v;
}

/* insert 'obj' object into storage hash */
void storage::InsertInHash( oid_t oid, value obj )
{
	//Why you need this?
  //       this->hashS.get_index ( oid, true );
  this->hashS[oid] = obj;
  //printf("st=%x value=%I64x oid=%x size=%d\n", this, obj, oid, this->hashS.size());
 
}

tool::string storage::GetNameByProto(VM* c, value proto)
{
  tool::string str = CsClassClassName(c,proto);
  hashNameProto[str] = proto;
  return str;
}

/* end of storage */

/* db_tripletTag */
db_tripletTag::db_tripletTag()
{
  data.s = 0;
  type = dybase_string_type;
  len = 0;
}
db_tripletTag::~db_tripletTag()
{
  if( dybase_string_type == type && data.s )   // dybase_string_type
  { delete[] data.s; }
}

void db_tripletTag::operator=(db_tripletTag& obj)
{
  type = obj.type;
  len = obj.len;
  if(dybase_string_type == type && len && obj.data.s) // not null string
  {
    data.s = new byte[len];
    ::memcpy( data.s, obj.data.s, len );
  }
  else
  { data = obj.data; }
}
/* end of db_tripletTag */


/* gc is about to run
  commit hash to storage...
*/
void StoragePreGC(VM* c, value vs )
{
  storage* s = (storage*)CsCObjectValue(vs);
  assert(s && s->dbS);

  if(s->autocommit)
  {
    s->CommitHash(c);
    dybase_commit( s->dbS );
  }

}

/* gc completed =>
  cleanup storage hash
*/
void StoragePostGC(VM* c, value& vs)
{
  storage* s = (storage*)CsCObjectValue(vs);
  assert(s && s->dbS);

  trace(L"Original Total number of objects in hash: %ld\n", s->hashS.size() );

  oid_t oid = 0;
  int i = 0;
	for(i = s->hashS.size() - 1; i >= 0 ; i--)
	{
		if( CsBrokenHeartP(s->hashS(i)) )
		{
			oid = ptr<persistent_header>(s->hashS(i))->oid;
			s->hashS[oid] = CsBrokenHeartForwardingAddr(s->hashS(i));
//trace(L"Broken heart: oid 0x%x\n", oid);
		}
		else
		{
			oid = ptr<persistent_header>(s->hashS(i))->oid;
			s->hashS.remove(oid);
//trace(L"Removing from hash: oid 0x%x\n", oid);
		}
	}

  trace(L"Total number of objects in storage hash: %ld\n", s->hashS.size() );

  // maintenance of hashNameProto
	for(i = s->hashNameProto.size() - 1; i >= 0 ; i--)
    s->hashNameProto(i) = CsCopyValue(c, s->hashNameProto(i) );


	/* restore storage object */
	if( s->hashS.size() && CsBrokenHeartP(vs))
	{ vs = CsBrokenHeartForwardingAddr(vs); }
}

bool IsEmptyStorage(value vs)
{
    storage* s = (storage*)CsCObjectValue(vs);
    return (0 == s->hashS.size());
}


/* globals */


// external interface
/*void detachObjFromStorage( value& obj )
{
	if( CsPersistentP(obj) && ptr<persistent_header>(obj)->vstorage )
	{
		assert( ptr<persistent_header>(obj)->oid );
		storage* s = (storage*)CsCObjectValue(ptr<persistent_header>(obj)->vstorage);
		assert( s );
		s->DetachObj(ptr<persistent_header>(obj)->oid );
	}
}
*/


/* error handler */
void errHandler(int error_code, char const* msg)
{
printf( "DyBase error: %d - '%s'\n", error_code, msg );
  //CsThrowKnownError(c, CsErrPersistError, msg);
}

value CsMakeStorage(VM* c, storage* s)
{
	return CsMakeCPtrObject(c, c->storageDispatch, s);
}


/* CsInitStorage - initialize the 'persistent' obj */
void CsInitStorage(VM *c)
{
	// create the 'Storage' type
	if (!(c->storageDispatch = CsEnterCPtrObjectType(CsGlobalScope(c), NULL, "Storage", methods, properties)))
	{ CsInsufficientMemory(c); }

	// setup alternate handlers
	c->storageDispatch->destroy = DestroyStorage;
}


/* usage:
    var s = storage.open(FileName[, autocommit])
*/
static value CSF_open(VM *c)
{
	wchar* wfname = NULL;
  bool autocommit = true;
	CsParseArguments(c, "**S|B", &wfname, &autocommit);

	storage* s = new storage();
	assert(s);
  s->autocommit = autocommit;

	if (!(s->dbS = dybase_open( wfname, 4*1024*1024, errHandler )))
	{
		delete s;
		return c->nullValue;
	}

  dybase_gc(s->dbS);

	value vs = CsMakeCPtrObject(c, c->storageDispatch, s);

	/* save new storage in VM::storages */
	c->storages.push(vs);
	return vs;
}

/* close storage
  and commit all attached objects beforehand
*/
static value CSF_close(VM *c)
{
	value val;
	CsParseArguments(c, "V=*", &val, c->storageDispatch);
	storage* s = (storage*)CsCObjectValue(val);
	if(!s || !s->dbS) { return c->falseValue; }

  if(s->autocommit)
    s->CommitHash(c);
  s->DetachAllObjs(c);

  // remove storage from the VM array
  int_t idx = c->storages.get_index(val);
  if( idx >= 0 )
  { c->storages.remove(idx); }

  /* storage will be closed in destructor */
	delete s;
	s = NULL;
trace( L"Storage closed\n" );

	CsSetCObjectValue(val, 0);
	return c->trueValue;
}


/* Close storage and destroy it
  NO storage hash commit */
void DestroyStorage(VM *c, value obj)
{
	storage* s = (storage*)CsCObjectValue(obj);

  if(s->autocommit)
    s->CommitHash(c);

  /* storage will be closed in destructor */
  // remove storage from the VM array
  int_t idx = c->storages.get_index(obj);
  if( idx >= 0 )
  { c->storages.remove(idx); }

	delete s;
	s = NULL;

  CsSetCObjectValue(obj, 0);
trace( L" Storage destroyed\n" );
}


/* commit storage hash
  & commit data to storage
*/
static value CSF_commit(VM *c)
{
	value val;
	storage* s;
	CsParseArguments(c, "V=*", &val, c->storageDispatch);
	s = (storage*)CsCObjectValue(val);
	if(!s || !s->dbS) { return c->falseValue; }

	s->CommitHash(c);

	dybase_commit( s->dbS );

trace( L"Storage commit\n" );

	return c->trueValue;
}

// clear all objects from storage cache
// => for all subsequent calls, objects will be loaded from Storage.
// Note: loaded / saved objects will retain oids and ref to the parent storage.
static value CSF_rollback(VM *c)
{
	value val;
	storage* s;
	CsParseArguments(c, "V=*", &val, c->storageDispatch);
	s = (storage*)CsCObjectValue(val);
	if(!s || !s->dbS) { return c->falseValue; }

  s->hashS.clear();

//	dybase_rollback( s->dbS );
trace( L"Storage rollback\n" );
	return c->trueValue;
}

/* return 'autocommit' property */
static value CSF_get_autocommit(VM *c, value vs)
{
	storage* s = (storage*)CsCObjectValue(vs);
	if(!s || !s->dbS)
	{
		CsThrowKnownError(c, CsErrPersistError, strErrCorruptPersistent);
		return c->falseValue;
	}

  return CsMakeBoolean( c, s->autocommit);
}

/* set 'autocommit' property */
/*static void CSF_set_autocommit(VM *c, value vs, value val)
{
	storage* s = (storage*)CsCObjectValue(vs);
	if(!s || !s->dbS)
		CsThrowKnownError(c, CsErrPersistError, strErrCorruptPersistent);

  s->autocommit = (CsBooleanP(c, val) && CsTrueP(c, val));
}*/

static value CSF_removeObject(VM *c)
{
	value obj;
	value val;
	storage* s;
	CsParseArguments(c, "V=*V", &obj, c->storageDispatch, &val);
  s = (storage*)CsCObjectValue(obj);
  if(!s || !s->dbS) { return c->falseValue; }

  if( !CsIsPersistInit(c, val) )
  { return c->falseValue; }

  value vsVal = ptr<persistent_header>(val)->vstorage;
	storage* sVal = (storage*)CsCObjectValue(vsVal);
  oid_t oidVal = ptr<persistent_header>(val)->oid;

  if( sVal == s )
  {
    dybase_deallocate_object(s->dbS, oidVal);
    s->hashS.remove(oidVal);
    return c->trueValue;
  }

	return c->falseValue;
}

/*
  return DbIndex object OR nullValue if can't create an Index
Usage:
  var intIndex = Storage.createIndex( #integer [,unique] );
*/
static value CSF_createIndex(VM *c)
{
  value val;
  storage* s;
  int  typeSym = 0;
  bool unique = true;
  CsParseArguments(c, "V=*L|B", &val, c->storageDispatch, &typeSym, &unique);
  s = (storage*)CsCObjectValue(val);
  if(!s || !s->dbS) { return c->nullValue; }

  dybase_oid_t oidIdx = 0;
  switch(typeSym)
  {
    case S_INTEGER:
      oidIdx = dybase_create_index(s->dbS, dybase_int_type, unique);
      break;

    case S_FLOAT:
      oidIdx = dybase_create_index(s->dbS, dybase_real_type, unique);
      break;

    case S_DATE:
      oidIdx = dybase_create_index(s->dbS, dybase_date_type, unique);
      break;

    case S_STRING:
      oidIdx = dybase_create_index(s->dbS, dybase_string_type, unique);
      break;

    default:
      CsThrowKnownError(c, CsErrUnexpectedTypeError, "supported types: int, float, string");
      break;
  }

  value obj = CsMakeDbIndex( c, val, oidIdx );

  return obj;
}


/*
  usage:
      s.registerClass( name, prototype );
  where:
      name        - is a string representation of a prototype
      prototype   - is a prototype of a persistent object
*/

/*static value CSF_registerClass(VM *c)
{
  value val;
  storage* s;
  wchar* protoName = NULL;
  value  proto;
  CsParseArguments(c, "V=*SV", &val, c->storageDispatch, &protoName, &proto);

  s = (storage*)CsCObjectValue(val);
  if(!s || !s->dbS || !protoName) { return c->falseValue; }

  // add proto name as the obj property
  CsSetObjectPropertyNoLoad( c, proto, symbol_value( _class ), CsMakeCString(c,protoName) );
  s->hashNameProto[protoName] = proto;

  return c->trueValue;
}*/

static value CSF_get_root(VM* c, value vs)
{
	storage* s = (storage*)CsCObjectValue(vs);
	if(!s || !s->dbS)
	{
		CsThrowKnownError(c, CsErrPersistError, strErrCorruptPersistent);
		return c->falseValue;
	}

	dybase_oid_t oid = dybase_get_root_object(s->dbS);
	if( !oid )
        return c->nullValue;

  return CsFetchObject(c,vs,oid);

  //value obj = CsMakeObject(c,c->undefinedValue);
  //ptr<persistent_header>(obj)->oid = oid;
  //ptr<persistent_header>(obj)->vstorage = vs;
  //ptr<persistent_header>(obj)->loaded(false); // clear loaded flag
  //return obj;
}

tool::string FetchClassName(storage* s, oid_t oid)
{
  assert(s && s->dbS);

	dybase_handle_t h = dybase_begin_load_object(s->dbS, oid);
	assert(h);

  char* strClassName = dybase_get_class_name(h);
  tool::string className = strClassName;

  dybase_end_load_object(h);

  return className;
}

value CsFetchObject( VM *c, value vs, oid_t oid )
{
    storage* s = (storage*)CsCObjectValue(vs);
    assert(s);

	  if( s->IsExistsInHash(oid) )
	  {
//      trace( L"\nobj returned from storage hash, oid = 0x%x\n", oid );
      return s->GetFromHash(oid);
	  }

    tool::string className = FetchClassName(s, oid);
    value proto = className.length() ? s->GetProtoByName(c,className) : 0;
    if(!proto) proto = c->objectObject;
		value obj = CsMakeObject( c, proto );

		// delayed obj loading
    ptr<persistent_header>(obj)->oid = oid;
    ptr<persistent_header>(obj)->vstorage = vs;
    ptr<persistent_header>(obj)->loaded(false); // clear loaded flag
    ptr<persistent_header>(obj)->modified(false); // clear modified flag

    s->InsertInHash(oid, obj);

    return obj;
}

value CsFetchVector( VM *c, value vs, dybase_oid_t oid )
{
    storage* s = (storage*)CsCObjectValue(vs);
    assert(s);

	  if( s->IsExistsInHash(oid) )
      return s->GetFromHash(oid);

    // load length for the vector
	  dybase_handle_t h = dybase_begin_load_object(s->dbS, oid);
	  assert(h);

	  char* className = dybase_get_class_name(h);
	  if( !className )
	  {
      assert(false);
      return c->falseValue;
    }

    char* fieldName = dybase_next_field(h);
    if( !fieldName )
	  {
      assert(false);
      return c->falseValue;
    }

    int_t type;
	  void* value_ptr = NULL;
	  int_t value_length = 0;
	  dybase_get_value(h, &type, &value_ptr, &value_length);
    dybase_end_load_object(h);

    value vec = CsMakeVector(c, value_length);

    ptr<persistent_header>(vec)->oid = oid;
    ptr<persistent_header>(vec)->vstorage = vs;
    ptr<persistent_header>(vec)->loaded(false); // clear loaded flag
    ptr<persistent_header>(vec)->modified(false); // clear modified flag

    s->InsertInHash(oid, vec);

    return vec;
}

// fetch object data
value  CsFetchObjectData( VM *c, value obj )
{
  if(ptr<persistent_header>(obj)->loaded())
    return obj; // already loaded, nothing to do.

  dybase_oid_t  oid = ptr<persistent_header>(obj)->oid;
  value         vs = ptr<persistent_header>(obj)->vstorage;
  storage*      s = (storage*)CsCObjectValue(vs);
//	assert(s && s->dbS);

  dybase_handle_t h = 0;
  try
  {
	  h = dybase_begin_load_object(s->dbS, oid);
	  assert(h);
  }
  catch(...)
  {
    CsThrowKnownError(c, CsErrPersistError, strErrCorruptPersistent);
  }

	char* className = dybase_get_class_name(h);
	if( !className )
	{
    assert(false);
    return obj;
  }

  char* fieldName = dybase_next_field(h);
  if( !fieldName )
	{
    assert(false);
    return obj;
  }

  int_t type;
	void* value_ptr = NULL;
	int_t value_length = 0;
	dybase_get_value(h, &type, &value_ptr, &value_length);

  assert( type == dybase_map_type );

  CsCheck(c,2);
  CsPush(c,obj); // FetchValue can allocate -> GC

	for( int_t i = 0; i < value_length; i++ )
	{
		  dybase_next_element( h );
		  value key = FetchValue( c, vs, h );
		  dybase_next_element( h );
      CsPush(c,key);
        value val = FetchValue( c, vs, h );
      key = CsPop(c);
      obj = CsTop(c);
		  CsSetObjectPropertyNoLoad( c, obj, key, val );
	}
  obj = CsPop(c);

  dybase_end_load_object(h);

  ptr<persistent_header>(obj)->loaded(true);
  ptr<persistent_header>(obj)->modified(false); // BUT NOT MODIFIED!

  return obj;

}

value  CsFetchVectorData( VM *c, value obj )
{
  if(ptr<persistent_header>(obj)->loaded())
    return obj; // already loaded, nothing to do.

  dybase_oid_t  oid = ptr<persistent_header>(obj)->oid;
  value         vs = ptr<persistent_header>(obj)->vstorage;
  storage*      s = (storage*)CsCObjectValue(vs);

	dybase_handle_t h = dybase_begin_load_object(s->dbS, oid);
	assert(h);

	char* className = dybase_get_class_name(h);
	if( !className )
	{
    assert(false);
    return obj;
  }

  char* fieldName = dybase_next_field(h);
  if( !fieldName )
	{
    assert(false);
    return obj;
  }

  int_t type;
	void* value_ptr = NULL;
	int_t value_length = 0;
	dybase_get_value(h, &type, &value_ptr, &value_length);

  assert( type == dybase_array_type );

  CsCheck(c,1);
  CsPush(c, obj);

  obj = CsResizeVectorNoLoad(c,obj,value_length);

	for( int_t i = 0; i < value_length; i++ )
	{
		dybase_next_element( h );
		value val = FetchValue( c, vs, h );
    obj = CsTop(c);
    CsSetVectorElementNoLoad(c, obj,i,val);
	}
  obj = CsPop(c);

  dybase_end_load_object(h);

  ptr<persistent_header>(obj)->loaded(true);
  ptr<persistent_header>(obj)->modified(false); // BUT NOT MODIFIED!

  return obj;
}

value FetchValue( VM *c, value vs, dybase_handle_t h )
{

  storage* s = (storage*)CsCObjectValue(vs);

	int_t type;
	void* value_ptr = NULL;
	int_t value_length = 0;
	dybase_get_value(h, &type, &value_ptr, &value_length);

	switch(type)
	{
	case dybase_object_ref_type:
		{
		  dybase_oid_t oid = *((dybase_oid_t*)value_ptr);
      return CsFetchObject(c, vs, oid);
		}
	case dybase_array_ref_type:
    {
      dybase_oid_t oid = *((dybase_oid_t*)value_ptr);
      return CsFetchVector(c, vs, oid);
    }
	case dybase_index_ref_type:
    {
      dybase_oid_t oid = *((dybase_oid_t*)value_ptr);
/* AM: it breaks reading of index from storage */
      if( !s->IsHashEmpty() && s->IsExistsInHash(oid) )
	    {
        trace( L"\nDbIndex obj returned from storage hash, oid = 0x%x\n", oid );
        return s->GetFromHash(oid);
	    }
      return CsMakeDbIndex(c, vs, oid);
    }
	case dybase_bool_type:
    return CsMakeBoolean(c, *((bool*)value_ptr) );

	case dybase_int_type:
		return CsMakeInteger(c,*((int_t*)value_ptr));

  // long here is a date
	case dybase_date_type:
		{
      datetime_t dt = *((datetime_t*)value_ptr);
      //FILETIME ft = ft64(i64);
      return CsMakeDate(c, dt);
//trace( L"date 0x%lx %lx\n", ft.dwHighDateTime, ft.dwLowDateTime );
		}

	case dybase_real_type:
		return CsMakeFloat(c,*(double*)value_ptr);
//trace( L"long %d - ", *((long*)value_ptr) );

	case dybase_string_type:
    {
      byte strType = *(byte*)value_ptr;
      switch(strType)
      {
      case db_char:
        {
          tool::string str( (char*)value_ptr + 1, value_length - 1);
          return CsMakeSymbol(c,str, str.length());
        }
      case db_wchar:
  		  return CsMakeCharString( c, (wchar*)((byte*)value_ptr + 1), (value_length-1)/2 );
      case db_blob:
        assert(false);
        break;
      default:
        assert(false);
        break;
      }
    }
//trace( L"string %s - ", (wchar*)value_ptr );
		break;

	case dybase_map_type:
    // element couldn't be a map
    // map =(by default) to object
		assert(false);
		break;

	default:
		assert(false);
		break;
	}

	return c->undefinedValue;
}


/* usage:
    storage.root = val;
*/
static void CSF_set_root(VM *c, value vs, value val)
{
	storage* s = (storage*)CsCObjectValue(vs);
	if(!s)
	{
		CsThrowKnownError(c, CsErrPersistError, strErrCorruptPersistent);
		return;
	}

  dybase_oid_t oid = 0;

  if( CsObjectP(val) )
    oid = CsSetPersistent( c, vs, val );
  else if( CsVectorP(val) || CsMovedVectorP(val) )
    oid = CsSetPersistent( c, vs, val );
  //CsVectorForwardingAddr(val)
  else
    CsThrowKnownError(c, CsErrUnexpectedTypeError, "root can be either object or array");

	if( oid )
	{
    dybase_set_root_object(s->dbS, oid);
  }
	else
	{ CsThrowKnownError(c, CsErrPersistError, "Can not save root object"); }

	return;
}

dybase_oid_t CsSetPersistent( VM *c, value vs, value obj )
{
	storage* s = (storage*)CsCObjectValue(vs);
  assert( s && s->dbS );

  dybase_oid_t oid;

  if( ptr<persistent_header>(obj)->vstorage == vs )
  {
    oid = ptr<persistent_header>(obj)->oid;
    if( !ptr<persistent_header>(obj)->modified() // if it is not modified
     &&  ptr<persistent_header>(obj)->loaded())  // and if it is loaded
    {
      // as it is already stored, nothing to do
      return oid;
    }
  }
  else // it is detached or attached to another storage

		oid = dybase_allocate_object( s->dbS );

  assert(oid);

  ptr<persistent_header>(obj)->oid = oid;
  ptr<persistent_header>(obj)->vstorage = vs;
  ptr<persistent_header>(obj)->loaded(true); // set loaded flag
  ptr<persistent_header>(obj)->modified(true); // and set modified flag

  s->InsertInHash(oid, obj);

  return oid;

}

dybase_oid_t CsStoreObject( VM *c, value vs, value obj )
{
  assert( CsObjectP(obj) );

  dybase_oid_t oid = CsSetPersistent(c, vs,obj);
  CsStoreObjectData( c, obj );

  return oid;
}


void CsStoreObjectData( VM *c, value obj )
{
  assert( CsObjectP(obj) );

  bool isPersistent = CsIsPersistent(c, obj);
  bool isModified = CsIsModified(obj);

  if( isPersistent && !isModified )
    return; // nothing to do

  value vs = ptr<persistent_header>(obj)->vstorage;

	storage* s = (storage*)CsCObjectValue(vs);
  assert( s && s->dbS );

  dybase_oid_t oid = ptr<persistent_header>(obj)->oid;

  ptr<persistent_header>(obj)->loaded(true); // set loaded flag
  ptr<persistent_header>(obj)->modified(false); // and clear modified flag

  value proto = CsObjectClass(obj);

  //tool::ustring uProtoName = s->GetNameByProto(c, proto);
  //tool::string str = uProtoName.utf8();
  //const char* strClassName = (uProtoName.length() ? str.c_str() : CsTypeName(obj) );

  tool::string strClassName = s->GetNameByProto(c, proto);

  dybase_handle_t h = dybase_begin_store_object( s->dbS, oid, strClassName );
	assert(h);
  if(!h) { CsThrowKnownError(c, CsErrPersistError, strErrCantSaveObj); }

		// -- store all properties --
	int countProperties = CsObjectPropertyCount(obj);
	// saving pairs: tag + value
	dybase_store_object_field(h, ".", dybase_map_type, 0, countProperties);

  // scan all obj properties and store every (key val) pair
  struct oscanner: object_scanner
  {
    dybase_handle_t h;
    value vs;
    bool item( VM *c, value key, value val )
    {
      StoreValue(c, vs, h,key);
      StoreValue(c, vs, h,val);
      return true; // continuue scan
    }
  } osc;

  osc.h = h;
  osc.vs = vs;

  CsScanObjectNoLoad(c,obj,osc);

		// end store obj
	dybase_end_store_object(h);

}

dybase_oid_t CsStoreVector( VM *c, value vs, value obj )
{
  assert( CsVectorP(obj) || CsMovedVectorP(obj) );


  dybase_oid_t oid = CsSetPersistent(c,vs,obj);
  CsStoreVectorData( c, obj );

  return oid;
}

void CsStoreVectorData( VM *c, value obj )
{

  dispatch* d = CsGetDispatch(obj);
  if( CsIsPersistent(c,obj) && !CsIsModified(obj) )
    return; // nothing to do

  const char* type_name = "Array";

#ifdef _DEBUG
  if(CsMovedVectorP(obj))
    type_name = type_name;
#endif 
  

  value vs = ptr<persistent_header>(obj)->vstorage;

	storage* s = (storage*)CsCObjectValue(vs);
  assert( s && s->dbS );

  dybase_oid_t oid = ptr<persistent_header>(obj)->oid;

  ptr<persistent_header>(obj)->loaded(true); // set loaded flag
  ptr<persistent_header>(obj)->modified(false); // and clear modified flag

  //CsVectorP(obj) || CsMovedVectorP(obj)

  dybase_handle_t h = dybase_begin_store_object( s->dbS, oid, type_name  );
	assert(h);
  if(!h) { CsThrowKnownError(c, CsErrPersistError, strErrCantSaveObj); }

	// -- store all elements --
 	int_t v_length = CsVectorSize(c,obj);
#ifdef _DEBUG
    if(v_length == 3)
      v_length = v_length;
#endif
	dybase_store_object_field(h, ".", dybase_array_type, 0, v_length);
	for( int_t i = 0; i < v_length; i++ )
	{
		value vel = CsVectorElement(c, obj, i );
		StoreValue( c, vs, h, vel );
	}
  dybase_end_store_object(h);

}


void Transform(VM* c, value vs, value val, db_triplet& db_v)
{
//trace( L"Transform: \n" );
	storage* s = (storage*)CsCObjectValue(vs);

	db_v.len = 0;
	db_v.data.i64 = 0;

  if( CsBooleanP(c, val) )
  {
    db_v.data.b = CsTrueP( c, val );
    db_v.type = dybase_bool_type;
  }
	else if( CsIntegerP( val ) )
	{
		db_v.data.i = to_int( val );
		db_v.type = dybase_int_type;
    //trace( L" int %d ", db_v.data.i );
	}
	else if( CsSymbolP( val ) )
	{
    tool::string str = CsSymbolName(val);
		db_v.len = 1 + str.length() * sizeof(char); // length in bytes + 1 byte for the type
    db_v.data.s = new byte[db_v.len];
    byte strType = db_char;
    ::memcpy( db_v.data.s, &strType, 1);
    ::memcpy( db_v.data.s + 1, (byte*)str.c_str(), db_v.len - 1);
		db_v.type = dybase_string_type;
	}
	else if( CsFloatP( val ) )
	{
		db_v.data.d = to_float( val );
		db_v.type = dybase_real_type;
//trace( L" float %u ", db_v.data.d );
	}
	else if( CsStringP( val ) )
	{
#pragma TODO("Alex, consider use of UTF8 here!")
		db_v.len = 1 + CsStringSize(val) * sizeof(wchar); // length in bytes + 1 byte for the type
    db_v.data.s = new byte[db_v.len];
    byte strType = db_wchar;
    ::memcpy( db_v.data.s, &strType, 1);
    ::memcpy( db_v.data.s + 1, (byte*)CsStringAddress(val), db_v.len - 1);
    db_v.type = dybase_string_type;
//trace( L" string %s ", (wchar*)db_v.data.s );
	}
  else if( CsDateP(c, val) )
	{
		datetime_t ft = CsDateValue(c,val);
		db_v.data.i64 = ft;
		db_v.type = dybase_date_type;
//trace( L" date 0x%lx %lx\n ", ft.dwHighDateTime, ft.dwLowDateTime );

	}
	else if( CsVectorP(val) || CsMovedVectorP(val) )
	{
		db_v.type = dybase_array_ref_type;
    db_v.data.oid = CsStoreVector( c, vs, val );
//		db_v.data.oid = StoreObj( c, vs, val );
//trace( L" vector 0x%x ", db_v.data.oid );
	}
  else if(CsDbIndexP(c, val))
  {
    db_v.type = dybase_index_ref_type;
    db_v.data.oid = ptr<persistent_header>(val)->oid;
  }
	else if(CsObjectP(val))
	{
    db_v.type = dybase_object_ref_type;
		db_v.data.oid = CsStoreObject( c, vs, val );
//trace( L" obj 0x%x ", db_v.data.oid );
	}
	else
	{
		// TODO: not supported?
trace( L"unknown type in Transform()\n");
		assert(false);
	}
//trace( L"End of Transform\n" );
	return;
}

void StoreValue( VM *c, value vs, dybase_handle_t h, value val)
{
	db_triplet db_val;
  Transform(c, vs, val, db_val);
	dybase_store_array_element(h,
          db_val.type,
         (db_val.type == dybase_string_type) ? (void*)db_val.data.s : &db_val.data,
          db_val.len);
}


}

//#pragma optimize( "", on )

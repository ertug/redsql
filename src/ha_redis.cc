#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

#define MYSQL_SERVER 1

#include <stdio.h>
#include "mysql_priv.h"
#include <mysql/plugin.h>

#include "util.h"
#include "ha_redis.h"
#include "redis.h"


static handler *redis_create_handler(handlerton *hton,
									 TABLE_SHARE *table, 
									 MEM_ROOT *mem_root);

handlerton *redis_hton;

/* Variables for redis share methods */

/* 
   Hash used to track the number of open tables; variable for redis share
   methods
*/
static HASH redis_open_tables;

/* The mutex used to init the hash; variable for redis share methods */
pthread_mutex_t redis_mutex;

/**
   @brief
   Function we use in the creation of our hash to get key.
*/

static uchar* redis_get_key(REDIS_SHARE *share, size_t *length,
							my_bool not_used __attribute__((unused)))
{
	*length=share->table_name_length;
	return (uchar*) share->table_name;
}


static int plugin_init(void *p)
{
	DBUG_ENTER("plugin_init");

	redis_hton= (handlerton *)p;
	VOID(pthread_mutex_init(&redis_mutex,MY_MUTEX_INIT_FAST));
	(void) hash_init(&redis_open_tables,system_charset_info,32,0,0,
					 (hash_get_key) redis_get_key,0,0);

	redis_hton->state=   SHOW_OPTION_YES;
	redis_hton->create=  redis_create_handler;
	redis_hton->flags=   HTON_CAN_RECREATE;

	redis_connect();

	DBUG_RETURN(0);
}


static int plugin_deinit(void *p)
{
	int error= 0;
	DBUG_ENTER("plugin_deinit");

	if (redis_open_tables.records)
		error= 1;
	hash_free(&redis_open_tables);
	pthread_mutex_destroy(&redis_mutex);

	DBUG_RETURN(error);
}


/**
   @brief
   Redis of simple lock controls. The "share" it creates is a
   structure we will pass to each redis handler. Do you have to have
   one of these? Well, you have pieces that are used for locking, and
   they are needed to function.
*/

static REDIS_SHARE *get_share(const char *table_name, TABLE *table)
{
	REDIS_SHARE *share;
	uint length;
	char *tmp_name;

	pthread_mutex_lock(&redis_mutex);
	length=(uint) strlen(table_name);

	if (!(share=(REDIS_SHARE*) hash_search(&redis_open_tables,
                                           (uchar*) table_name,
                                           length)))
	{
		if (!(share=(REDIS_SHARE *)
			  my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
							  &share, sizeof(*share),
							  &tmp_name, length+1,
							  NullS)))
		{
			pthread_mutex_unlock(&redis_mutex);
			return NULL;
		}

		share->use_count=0;
		share->table_name_length=length;
		share->table_name=tmp_name;
		strmov(share->table_name,table_name);
		if (my_hash_insert(&redis_open_tables, (uchar*) share))
			goto error;
		thr_lock_init(&share->lock);
		pthread_mutex_init(&share->mutex,MY_MUTEX_INIT_FAST);
	}
	share->use_count++;
	pthread_mutex_unlock(&redis_mutex);

	return share;

error:
	pthread_mutex_destroy(&share->mutex);
	my_free(share, MYF(0));

	return NULL;
}


/**
   @brief
   Free lock controls. We call this whenever we close a table. If the table had
   the last reference to the share, then we free memory associated with it.
*/

static int free_share(REDIS_SHARE *share)
{
	pthread_mutex_lock(&redis_mutex);
	if (!--share->use_count)
	{
		hash_delete(&redis_open_tables, (uchar*) share);
		thr_lock_delete(&share->lock);
		pthread_mutex_destroy(&share->mutex);
		my_free(share, MYF(0));
	}
	pthread_mutex_unlock(&redis_mutex);

	return 0;
}

static handler* redis_create_handler(handlerton *hton,
									 TABLE_SHARE *table, 
									 MEM_ROOT *mem_root)
{
	return new (mem_root) ha_redis(hton, table);
}

ha_redis::ha_redis(handlerton *hton, TABLE_SHARE *table_arg)
	:handler(hton, table_arg)
{}


/**
   @brief
   If frm_error() is called then we will use this to determine
   the file extensions that exist for the storage engine. This is also
   used by the default rename_table and delete_table method in
   handler.cc.

   For engines that have two file name extentions (separate meta/index file
   and data file), the order of elements is relevant. First element of engine
   file name extentions array should be meta/index file extention. Second
   element - data file extention. This order is assumed by
   prepare_for_repair() when REPAIR TABLE ... USE_FRM is issued.

   @see
   rename_table method in handler.cc and
   delete_table method in handler.cc
*/

static const char *ha_redis_exts[] = {
	NullS
};

const char **ha_redis::bas_ext() const
{
	return ha_redis_exts;
}


/**
   @brief
   Used for opening tables. The name will be the name of the file.

   @details
   A table is opened when it needs to be opened; e.g. when a request comes in
   for a SELECT on the table (tables are not open and closed for each request,
   they are cached).

   Called from handler.cc by handler::ha_open(). The server opens all tables by
   calling ha_open() which then calls the handler specific open().

   @see
   handler::ha_open() in handler.cc
*/

int ha_redis::open(const char *name, int mode, uint test_if_locked)
{
	DBUG_ENTER("ha_redis::open");

	if (!(share = get_share(name, table)))
		DBUG_RETURN(1);
	thr_lock_data_init(&share->lock,&lock,NULL);

	DBUG_RETURN(0);
}


/**
   @brief
   Closes a table. We call the free_share() function to free any resources
   that we have allocated in the "shared" structure.

   @details
   Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc it is
   only used to close up temporary tables or during the process where a
   temporary table is converted over to being a myisam table.

   For sql_base.cc look at close_data_tables().

   @see
   sql_base.cc, sql_select.cc and table.cc
*/

int ha_redis::close(void)
{
	DBUG_ENTER("ha_redis::close");
	DBUG_RETURN(free_share(share));
}

int ha_redis::write_row(uchar *record)
{
	DBUG_ENTER("ha_redis::write_row");

	if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_UPDATE)
		table->timestamp_field->set_time();
	if (table->next_number_field && record == table->record[0])
		update_auto_increment();

	char *tablename = (char*)my_malloc(strlen(share->table_name) * sizeof(char), MYF(MY_FAE));
	extract_table_name(tablename, share->table_name);

	fprintf(stderr, "write_row:%s\n", tablename);

	// TODO: redis transacations
	llong rid = redis_write_row(tablename);

	for (Field **field=table->field; *field; field++) {
		char *fieldname = (char*)my_malloc(strlen((*field)->field_name) * sizeof(char), MYF(MY_FAE));
		strcpy(fieldname, (*field)->field_name);

		int length = (*field)->pack_length();
		uint offset = (*field)->offset(record);
		enum_field_types type = (*field)->type();

		uchar *orig_value = record + offset + 1;
		uchar *value = (uchar*)my_malloc(length * sizeof(uchar), MYF(MY_FAE));
		int i = 0;
		switch ((*field)->type()) {
		case MYSQL_TYPE_VARCHAR:
		case MYSQL_TYPE_VAR_STRING:
		case MYSQL_TYPE_STRING:
			// store just the part until null terminator (strcpy is not working on uchar)
			for (uchar *ptr = orig_value; ptr != '\0'; ptr++) {
				value[i++] = *ptr;
			}
			break;
		default:
			memcpy(value, orig_value, length * sizeof(uchar));
		}

		fprintf(stderr, "field: name:%s, type:%d, value:%s, pack_len:%d, offset:%d\n", fieldname, type, value, length, offset);

		redis_write_field(tablename, rid, fieldname, value, length);

		my_free(fieldname, MYF(MY_FAE));
		my_free(value, MYF(MY_FAE));
	}
	my_free(tablename, MYF(MY_FAE));

	DBUG_RETURN(0);
}


/**
   @brief
   Yes, update_row() does what you expect, it updates a row. old_data will have
   the previous row record in it, while new_data will have the newest data in it.
   Keep in mind that the server can do updates based on ordering if an ORDER BY
   clause was used. Consecutive ordering is not guaranteed.

   @details
   Currently new_data will not have an updated auto_increament record, or
   and updated timestamp field. You can do these for redis by doing:
   @code
   if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_UPDATE)
   table->timestamp_field->set_time();
   if (table->next_number_field && record == table->record[0])
   update_auto_increment();
   @endcode

   Called from sql_select.cc, sql_acl.cc, sql_update.cc, and sql_insert.cc.

   @see
   sql_select.cc, sql_acl.cc, sql_update.cc and sql_insert.cc
*/
int ha_redis::update_row(const uchar *old_data, uchar *new_data)
{

	DBUG_ENTER("ha_redis::update_row");
	/*
	if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_UPDATE)
		table->timestamp_field->set_time();
	if (table->next_number_field && record == table->record[0])
	   update_auto_increment();
	*/
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
   @brief
   This will delete a row. buf will contain a copy of the row to be deleted.
   The server will call this right after the current row has been called (from
   either a previous rnd_nexT() or index call).

   @details
   If you keep a pointer to the last row or can access a primary key it will
   make doing the deletion quite a bit easier. Keep in mind that the server does
   not guarantee consecutive deletions. ORDER BY clauses can be used.

   Called in sql_acl.cc and sql_udf.cc to manage internal table
   information.  Called in sql_delete.cc, sql_insert.cc, and
   sql_select.cc. In sql_select it is used for removing duplicates
   while in insert it is used for REPLACE calls.

   @see
   sql_acl.cc, sql_udf.cc, sql_delete.cc, sql_insert.cc and sql_select.cc
*/

int ha_redis::delete_row(const uchar *buf)
{
	DBUG_ENTER("ha_redis::delete_row");
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
   @brief
   Positions an index cursor to the index specified in the handle. Fetches the
   row if available. If the key value is null, begin at the first key of the
   index.
*/

int ha_redis::index_read_map(uchar *buf, const uchar *key,
							 key_part_map keypart_map __attribute__((unused)),
							 enum ha_rkey_function find_flag
							 __attribute__((unused)))
{
	DBUG_ENTER("ha_redis::index_read");
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
   @brief
   Used to read forward through the index.
*/

int ha_redis::index_next(uchar *buf)
{
	DBUG_ENTER("ha_redis::index_next");
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
   @brief
   Used to read backwards through the index.
*/

int ha_redis::index_prev(uchar *buf)
{
	DBUG_ENTER("ha_redis::index_prev");
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
   @brief
   index_first() asks for the first key in the index.

   @details
   Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

   @see
   opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_redis::index_first(uchar *buf)
{
	DBUG_ENTER("ha_redis::index_first");
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
   @brief
   index_last() asks for the last key in the index.

   @details
   Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

   @see
   opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_redis::index_last(uchar *buf)
{
	DBUG_ENTER("ha_redis::index_last");
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
   @brief
   rnd_init() is called when the system wants the storage engine to do a table
   scan. See the redis in the introduction at the top of this file to see when
   rnd_init() is called.

   @details
   Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
   and sql_update.cc.

   @see
   filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_redis::rnd_init(bool scan)
{
	DBUG_ENTER("ha_redis::rnd_init");
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_redis::rnd_end()
{
	DBUG_ENTER("ha_redis::rnd_end");
	DBUG_RETURN(0);
}


/**
   @brief
   This is called for each row of the table scan. When you run out of records
   you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
   The Field structure for the table is the key to getting data into buf
   in a manner that will allow the server to understand it.

   @details
   Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
   and sql_update.cc.

   @see
   filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_redis::rnd_next(uchar *buf)
{
	DBUG_ENTER("ha_redis::rnd_next");
	DBUG_RETURN(HA_ERR_END_OF_FILE);
}


/**
   @brief
   position() is called after each call to rnd_next() if the data needs
   to be ordered. You can do something like the following to store
   the position:
   @code
   my_store_ptr(ref, ref_length, current_position);
   @endcode

   @details
   The server uses ref to store data. ref_length in the above case is
   the size needed to store current_position. ref is just a byte array
   that the server will maintain. If you are using offsets to mark rows, then
   current_position should be the offset. If it is a primary key like in
   BDB, then it needs to be a primary key.

   Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

   @see
   filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
*/
void ha_redis::position(const uchar *record)
{
	DBUG_ENTER("ha_redis::position");
	DBUG_VOID_RETURN;
}


/**
   @brief
   This is like rnd_next, but you are given a position to use
   to determine the row. The position will be of the type that you stored in
   ref. You can use ha_get_ptr(pos,ref_length) to retrieve whatever key
   or position you saved when position() was called.

   @details
   Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and sql_update.cc.

   @see
   filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
*/
int ha_redis::rnd_pos(uchar *buf, uchar *pos)
{
	DBUG_ENTER("ha_redis::rnd_pos");
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
   @brief
   ::info() is used to return information to the optimizer. See my_base.h for
   the complete description.

   @details
   Currently this table handler doesn't implement most of the fields really needed.
   SHOW also makes use of this data.

   You will probably want to have the following in your code:
   @code
   if (records < 2)
   records = 2;
   @endcode
   The reason is that the server will optimize for cases of only a single
   record. If, in a table scan, you don't know the number of records, it
   will probably be better to set records to two so you can return as many
   records as you need. Along with records, a few more variables you may wish
   to set are:
   records
   deleted
   data_file_length
   index_file_length
   delete_length
   check_time
   Take a look at the public variables in handler.h for more information.

   Called in filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
   sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
   sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc,
   sql_table.cc, sql_union.cc, and sql_update.cc.

   @see
   filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc, sql_delete.cc,
   sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc, sql_select.cc,
   sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_table.cc,
   sql_union.cc and sql_update.cc
*/
int ha_redis::info(uint flag)
{
	DBUG_ENTER("ha_redis::info");
	DBUG_RETURN(0);
}


/**
   @brief
   extra() is called whenever the server wishes to send a hint to
   the storage engine. The myisam engine implements the most hints.
   ha_innodb.cc has the most exhaustive list of these hints.

   @see
   ha_innodb.cc
*/
int ha_redis::extra(enum ha_extra_function operation)
{
	DBUG_ENTER("ha_redis::extra");
	DBUG_RETURN(0);
}


/**
   @brief
   Used to delete all rows in a table, including cases of truncate and cases where
   the optimizer realizes that all rows will be removed as a result of an SQL statement.

   @details
   Called from item_sum.cc by Item_func_group_concat::clear(),
   Item_sum_count_distinct::clear(), and Item_func_group_concat::clear().
   Called from sql_delete.cc by mysql_delete().
   Called from sql_select.cc by JOIN::reinit().
   Called from sql_union.cc by st_select_lex_unit::exec().

   @see
   Item_func_group_concat::clear(), Item_sum_count_distinct::clear() and
   Item_func_group_concat::clear() in item_sum.cc;
   mysql_delete() in sql_delete.cc;
   JOIN::reinit() in sql_select.cc and
   st_select_lex_unit::exec() in sql_union.cc.
*/
int ha_redis::delete_all_rows()
{
	DBUG_ENTER("ha_redis::delete_all_rows");
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
   @brief
   This create a lock on the table. If you are implementing a storage engine
   that can handle transacations look at ha_berkely.cc to see how you will
   want to go about doing this. Otherwise you should consider calling flock()
   here. Hint: Read the section "locking functions for mysql" in lock.cc to understand
   this.

   @details
   Called from lock.cc by lock_external() and unlock_external(). Also called
   from sql_table.cc by copy_data_between_tables().

   @see
   lock.cc by lock_external() and unlock_external() in lock.cc;
   the section "locking functions for mysql" in lock.cc;
   copy_data_between_tables() in sql_table.cc.
*/
int ha_redis::external_lock(THD *thd, int lock_type)
{
	DBUG_ENTER("ha_redis::external_lock");
	DBUG_RETURN(0);
}


/**
   @brief
   The idea with handler::store_lock() is: The statement decides which locks
   should be needed for the table. For updates/deletes/inserts we get WRITE
   locks, for SELECT... we get read locks.

   @details
   Before adding the lock into the table lock handler (see thr_lock.c),
   mysqld calls store lock with the requested locks. Store lock can now
   modify a write lock to a read lock (or some other lock), ignore the
   lock (if we don't want to use MySQL table locks at all), or add locks
   for many tables (like we do when we are using a MERGE handler).

   Berkeley DB, for redis, changes all WRITE locks to TL_WRITE_ALLOW_WRITE
   (which signals that we are doing WRITES, but are still allowing other
   readers and writers).

   When releasing locks, store_lock() is also called. In this case one
   usually doesn't have to do anything.

   In some exceptional cases MySQL may send a request for a TL_IGNORE;
   This means that we are requesting the same lock as last time and this
   should also be ignored. (This may happen when someone does a flush
   table when we have opened a part of the tables, in which case mysqld
   closes and reopens the tables and tries to get the same locks at last
   time). In the future we will probably try to remove this.

   Called from lock.cc by get_lock_data().

   @note
   In this method one should NEVER rely on table->in_use, it may, in fact,
   refer to a different thread! (this happens if get_lock_data() is called
   from mysql_lock_abort_for_thread() function)

   @see
   get_lock_data() in lock.cc
*/
THR_LOCK_DATA **ha_redis::store_lock(THD *thd,
									 THR_LOCK_DATA **to,
									 enum thr_lock_type lock_type)
{
	if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
		lock.type=lock_type;
	*to++= &lock;
	return to;
}


/**
   @brief
   Used to delete a table. By the time delete_table() has been called all
   opened references to this table will have been closed (and your globally
   shared references released). The variable name will just be the name of
   the table. You will need to remove any files you have created at this point.

   @details
   If you do not implement this, the default delete_table() is called from
   handler.cc and it will delete all files with the file extensions returned
   by bas_ext().

   Called from handler.cc by delete_table and ha_create_table(). Only used
   during create if the table_flag HA_DROP_BEFORE_CREATE was specified for
   the storage engine.

   @see
   delete_table and ha_create_table() in handler.cc
*/
int ha_redis::delete_table(const char *name)
{
	DBUG_ENTER("ha_redis::delete_table");
	/* This is not implemented but we want someone to be able that it works. */
	DBUG_RETURN(0);
}


/**
   @brief
   Renames a table from one name to another via an alter table call.

   @details
   If you do not implement this, the default rename_table() is called from
   handler.cc and it will delete all files with the file extensions returned
   by bas_ext().

   Called from sql_table.cc by mysql_rename_table().

   @see
   mysql_rename_table() in sql_table.cc
*/
int ha_redis::rename_table(const char * from, const char * to)
{
	DBUG_ENTER("ha_redis::rename_table ");
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
   @brief
   Given a starting key and an ending key, estimate the number of rows that
   will exist between the two keys.

   @details
   end_key may be empty, in which case determine if start_key matches any rows.

   Called from opt_range.cc by check_quick_keys().

   @see
   check_quick_keys() in opt_range.cc
*/
ha_rows ha_redis::records_in_range(uint inx, key_range *min_key,
								   key_range *max_key)
{
	DBUG_ENTER("ha_redis::records_in_range");
	DBUG_RETURN(10);                         // low number to force index usage
}


/**
   @brief
   create() is called to create a database. The variable name will have the name
   of the table.

   @details
   When create() is called you do not need to worry about
   opening the table. Also, the .frm file will have already been
   created so adjusting create_info is not necessary. You can overwrite
   the .frm file at this point if you wish to change the table
   definition, but there are no methods currently provided for doing
   so.

   Called from handle.cc by ha_create_table().

   @see
   ha_create_table() in handle.cc
*/

int ha_redis::create(const char *name, TABLE *table_arg,
					 HA_CREATE_INFO *create_info)
{
	DBUG_ENTER("ha_redis::create");
	/*
	  This is not implemented but we want someone to be able to see that it
	  works.
	*/
	DBUG_RETURN(0);
}


struct st_mysql_storage_engine redis_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

static ulong srv_enum_var= 0;
static ulong srv_ulong_var= 0;

const char *enum_var_names[]=
{
	"e1", "e2", NullS
};

TYPELIB enum_var_typelib=
{
	array_elements(enum_var_names) - 1, "enum_var_typelib",
	enum_var_names, NULL
};

static MYSQL_SYSVAR_ENUM(
	enum_var,                       // name
	srv_enum_var,                   // varname
	PLUGIN_VAR_RQCMDARG,            // opt
	"Sample ENUM system variable.", // comment
	NULL,                           // check
	NULL,                           // update
	0,                              // def
	&enum_var_typelib);             // typelib

static MYSQL_SYSVAR_ULONG(
	ulong_var,
	srv_ulong_var,
	PLUGIN_VAR_RQCMDARG,
	"0..1000",
	NULL,
	NULL,
	8,
	0,
	1000,
	0);

static struct st_mysql_sys_var* redis_system_variables[]= {
	MYSQL_SYSVAR(enum_var),
	MYSQL_SYSVAR(ulong_var),
	NULL
};

mysql_declare_plugin(redis)
{
	MYSQL_STORAGE_ENGINE_PLUGIN,
		&redis_storage_engine,
		"REDIS",
		"Ertug Karamatli",
		"Redis storage engine",
		PLUGIN_LICENSE_GPL,
		plugin_init,                            /* Plugin Init */
		plugin_deinit,                            /* Plugin Deinit */
		0x0001 /* 0.1 */,
		NULL,                                         /* status variables */
		redis_system_variables,                     /* system variables */
		NULL                                          /* config options */
		}
mysql_declare_plugin_end;

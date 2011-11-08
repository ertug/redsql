#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mysql_priv.h"

#include "redis.h"
#include "hiredis.h"

redisContext *c;


void redis_cleanup()
{
	redisFree(c);
}

int redis_connect()
{
	
    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
	fprintf(stderr, "Connecting\n");
    c = redisConnectWithTimeout((char*)"127.0.0.1", 6379, timeout);
    if (c->err) {
        fprintf(stderr, "Connection error: %s\n", c->errstr);
		redis_cleanup();
        return REDIS_ERR;
    }
	return REDIS_OK;
}

// low-level wrappers -----

int check_error(redisReply *reply)
{
	if (reply->type == REDIS_REPLY_ERROR) {
		fprintf(stderr, "REDIS ERROR: %s\n", reply->str);
		redis_connect();
		return REDIS_ERR;
	}
}

int redis_set(char *key, uchar *val, uint vallen)
{
	redisReply *reply = (redisReply*)redisCommand(c, "SET %s %b", key, val, vallen);
	if (check_error(reply) == REDIS_ERR) {
		freeReplyObject(reply);
		return REDIS_ERR;
	}

	fprintf(stderr, "SET: %s\n", reply->str);
	freeReplyObject(reply);
}

int redis_rpush(char *key, char *val, uint vallen)
{
	redisReply *reply = (redisReply*)redisCommand(c, "RPUSH %s %b", key, val, vallen);
	if (check_error(reply) == REDIS_ERR) {
		freeReplyObject(reply);
		return REDIS_ERR;
	}

	fprintf(stderr, "RPUSH: %s\n", reply->str);
	freeReplyObject(reply);
}

llong redis_incr(char *key)
{
	redisReply *reply = (redisReply*)redisCommand(c, "INCR %s", key);
	if (check_error(reply) == REDIS_ERR) {
		freeReplyObject(reply);
		return REDIS_ERR;
	}

	fprintf(stderr, "INCR: %lld\n", reply->integer);
	llong res = reply->integer;
	freeReplyObject(reply);
	return res;
}

// -----

void lltoa(char *str, llong num)
{
	sprintf(str, "%lld", num);
}

llong redis_write_row(const char *tablename)
{
	int keylen;
	char *key;

	keylen = strlen(tablename) + 8;
	key = (char*)my_malloc(keylen * sizeof(char), MYF(MY_FAE));
	sprintf(key, "%s:lastrid", tablename);
	llong rid = redis_incr(key);
	my_free(key, MYF(MY_FAE));

	keylen = strlen(tablename) + 4;
	key = (char*)my_malloc(keylen * sizeof(char), MYF(MY_FAE));
	sprintf(key, "%s:rid", tablename);
	char ridstr[20];
	lltoa(ridstr, rid);
	int resp = redis_rpush(key, ridstr, strlen(ridstr));
	my_free(key, MYF(MY_FAE));

	return rid;
}

int redis_write_field(const char *tablename, llong rid, const char *fieldname, uchar *val, uint vallen)
{
	char ridstr[20];
	lltoa(ridstr, rid);

	// XXX: adding 2 for semicolons
	int keylen = strlen(tablename) + strlen(fieldname) + strlen(ridstr) + 2;
	char *key = (char*)my_malloc(keylen * sizeof(char), MYF(MY_FAE));

	sprintf(key, "%s:%s:%s", tablename, ridstr, fieldname);

	int resp = redis_set(key, val, vallen);

	my_free(key, MYF(MY_FAE));
}

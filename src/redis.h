#define REDIS_ERR -1
#define REDIS_OK 0

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int uint;
typedef unsigned char uchar;
typedef long long llong;

int redis_connect();
void redis_cleanup();
llong redis_write_row(const char *tablename);
int redis_write_field(const char *tablename, llong rid, const char *fieldname, uchar *val, uint vallen);

#ifdef __cplusplus
}
#endif

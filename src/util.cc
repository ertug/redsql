#include <string.h>
#include <strend.c>
#include "util.h"

void extract_table_name(char *table_name, const char *name)
{
	char *ptr = strend(name) - 1;
	while (ptr >= name && *ptr != '\\' && *ptr != '/') {
		ptr--;
	}
	char *name_ptr = ptr + 1;

	memcpy(table_name, name_ptr, (size_t)(strend(name) - (int)name_ptr + 1));
}

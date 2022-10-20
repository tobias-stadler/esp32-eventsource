#ifndef DEFUTIL_H
#define DEFUTIL_H

#include <string.h>

/**
 *
 * Useful defines and functions
 *
 */

//Strings
#define ENDS_WITH(str, ext) (strcasecmp(&str[strlen(str) - sizeof(ext) + 1], ext) == 0)
#define STARTS_WITH(str, start) (!strncasecmp(str, start, strlen(start)))

#endif

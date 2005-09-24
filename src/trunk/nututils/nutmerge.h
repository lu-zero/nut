#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
//#define NDEBUG
#include <assert.h>
#include <nut.h>

#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define MAX(a,b) ((a) < (b) ? (b) : (a))

#define FREAD(file, len, var) do { if (fread((var), 1, (len), (file)) != (len)) return 1; }while(0)

extern FILE * stats;

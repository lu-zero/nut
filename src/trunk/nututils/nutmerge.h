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

struct demuxer_t {
	char * extention;
	void * (*init)(FILE * in); ///< returns priv
	/// nut_streams must be free()'d!! nut_streams becomes invalid after uninit!!
	int (*read_headers)(void * priv, nut_stream_header_t ** nut_streams);
	int (*get_packet)(void * priv, nut_packet_t * p);
	void (*uninit)(void * priv);
};

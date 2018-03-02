/*
 * Please do not edit this file.
 * It was generated using rpcgen.
 */

#ifndef _TYPES_H_RPCGEN
#define _TYPES_H_RPCGEN

#include <rpc/rpc.h>


#ifdef __cplusplus
extern "C" {
#endif


enum tagtype {
	GET = 0,
	OK = 1,
	QUIT = 2,
	ERR = 3,
};
typedef enum tagtype tagtype; //nuovo tipo "tagtype"

struct file {
	struct {
		u_int contents_len;
		char *contents_val;
	} contents;
	u_int last_mod_time;
};
typedef struct file file; //nuovo tipo "file"

struct message {
	tagtype tag;
	union {
		char *filename;
		struct file fdata;
	} message_u;
};
typedef struct message message; //nuovo tipo "message"

/* the xdr functions */

#if defined(__STDC__) || defined(__cplusplus)
extern  bool_t xdr_tagtype (XDR *, tagtype*);
extern  bool_t xdr_file (XDR *, file*);
extern  bool_t xdr_message (XDR *, message*);

#else /* K&R C */
extern bool_t xdr_tagtype ();
extern bool_t xdr_file ();
extern bool_t xdr_message ();

#endif /* K&R C */

#ifdef __cplusplus
}
#endif

#endif /* !_TYPES_H_RPCGEN */
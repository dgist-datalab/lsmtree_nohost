#ifndef __SKIPLIST_HEADER
#define __SKIPLIST_HEADER
#define MAX_L 30
#include"utils.h"
#include"stdint.h"
#include"LR_inter.h"
#include"bloomfilter.h"
typedef struct lsmtree_req_t lsmtree_req_t;
typedef struct lsmtree_gc_req_t lsmtree_gc_req_t;
typedef struct snode{
	KEYT key;
	KEYT ppa;
	int level;
	char *value;
	bool vflag;
	struct lsmtree_req_t *req;
	struct snode **list;
}snode;

typedef struct skiplist{
	uint8_t level;
	KEYT start,end;
	uint64_t size;
	snode *header;
	uint8_t *bitset;
#ifdef BLOOM
	BF *filter;
#endif
}skiplist;

typedef struct skIterator{
	skiplist *mylist;
	snode *now;
} skIterator;

typedef struct keyset{
	KEYT key;
	KEYT ppa;
}keyset;

typedef struct sktable{
	keyset meta[KEYN];
	char *value;
}sktable;

snode *snode_init(snode*);
skiplist *skiplist_init(skiplist*);
snode *skiplist_find(skiplist*,KEYT);
snode *skiplist_insert(skiplist*,KEYT,char *,struct lsmtree_req_t *,bool);

sktable *skiplist_read(KEYT, int hfd, int dfd);
sktable *skiplist_meta_read_n(KEYT, int fd,int ,struct lsmtree_req_t *);
sktable *skiplist_meta_read_c(KEYT, int fd,int ,struct lsmtree_gc_req_t *);
sktable *skiplist_data_read(sktable*,KEYT pbn, int fd);
keyset* skiplist_keyset_find(sktable *,KEYT key);
bool skiplist_keyset_read(keyset* ,char *,int fd,lsmtree_req_t *);
bool skiplist_keyset_read_c(keyset* ,char *,int fd,lsmtree_gc_req_t *);
void skiplist_sktable_free(sktable *);

snode *skiplist_pop(skiplist *);
KEYT skiplist_write(skiplist*,lsmtree_gc_req_t *,int hfd, int dfd,double fpr);
KEYT skiplist_meta_write(skiplist *, int fd,struct lsmtree_gc_req_t*,double fpr);
KEYT skiplist_data_write(skiplist *, int fd,struct lsmtree_gc_req_t*);
void skiplist_sk_data_write(sktable *,int fd,struct lsmtree_gc_req_t*);
skiplist* skiplist_cut(skiplist *,KEYT num,KEYT limit);
void skiplist_ex_value_free(skiplist *list);
void skiplist_meta_free(skiplist *list);
void skiplist_free(skiplist *list);
KEYT sktable_meta_write(sktable* input,lsmtree_gc_req_t *,int dfd,BF **filter,float fpr);
skIterator* skiplist_getIterator(skiplist *list);
void skiplist_traversal(skiplist *data);
void sktable_print(sktable *);
bool sktable_check(sktable*);
sktable *skiplist_to_sk(skiplist *);
sktable *skiplist_to_sk_extra(skiplist *,float fpr,uint8_t**,BF**);

void skiplist_save(skiplist *,int fd);
void skiplist_load(skiplist*, int fd);
void skiplist_relocate_data(skiplist *);
#endif

#include"bptree.h"
#include"lsmtree.h"
#include"skiplist.h"
#include"measure.h"
#include"threading.h"
#include"LR_inter.h"
#include"delete_set.h"
#include"bloomfilter.h"
#include<time.h>
#include<pthread.h>
#include<string.h>
#include<stdio.h>
#include<stdlib.h>
#include<fcntl.h>
#include<semaphore.h>
#include<sys/time.h>
#include<limits.h>
#include<math.h>
#ifdef ENABLE_LIBFTL
#include "libmemio.h"
memio_t* mio;
#endif

#ifdef CACHE
extern	cache *CH;
#endif

#ifdef THREAD
extern threadset processor;
extern MeasureTime mt;
extern uint64_t* oob;
extern delete_set *header_segment;
extern delete_set *data_segment;
extern KEYT DELETEDKEY;
pthread_mutex_t sst_lock;
pthread_mutex_t mem_lock;

MeasureTime mem;
MeasureTime last;
MeasureTime buf;
MeasureTime bp;
MeasureTime find;

int cache_hit;
int pros_hit2;
int pros_hit;
int mem_hit;
int header_read;

extern int write_make_check;
extern int write_wait_check;
/*
0: memtable
1~LEVELN : level
LEVELN+1=last
*/
#endif


KEYT write_data(lsmtree *LSM,skiplist *input,lsmtree_gc_req_t *req,double fpr){
	return skiplist_write(input,req,LSM->dfd,LSM->dfd,fpr);
}
int write_meta_only(lsmtree *LSM, skiplist *data,lsmtree_gc_req_t * input,double fpr){
	return skiplist_meta_write(data,LSM->dfd,input,fpr);
}

lsmtree* init_lsm(lsmtree *res){
	pthread_mutex_init(&sst_lock,NULL);
	pthread_mutex_init(&mem_lock,NULL);

	measure_init(&mem);
	measure_init(&last);
	measure_init(&buf);
	measure_init(&bp);
	measure_init(&find);

	res->memtree=(skiplist*)malloc(sizeof(skiplist));
	res->memtree=skiplist_init(res->memtree);
	res->buf.data=NULL;
	res->sstable=NULL;
	res->buf.lastB=NULL;

	//setting fprs
	double ffpr=RAF*(1-MUL)/(1-pow(MUL,LEVELN+0));
	uint64_t bits=0;
	//
	for(int i=0;i<LEVELN; i++){
		res->buf.disk[i]=(level*)malloc(sizeof(level));
		if(i==0)
			level_init(res->buf.disk[i],MUL);
		else
			level_init(res->buf.disk[i],res->buf.disk[i-1]->m_size*MUL);
		double target_fpr=pow(MUL,i)*ffpr;
		if(target_fpr>1)
			res->buf.disk[i]=0;
		else{
#ifdef MONKEY_BLOOM
			res->buf.disk[i]->fpr=target_fpr;
#else
			res->buf.disk[i]->fpr=FPR;//pow(MUL,i)*ffpr;
#endif
			printf("[%d] : fpr > %f\n",i,res->buf.disk[i]->fpr);
			bits+=bf_bits(pow(MUL,i)*KEYN,FPR);
		}
		//	printf("[%d]%lf\n",i+1,target_fpr);
	}
	//printf("bits : %lu\n",bits);
#if !defined(ENABLE_LIBFTL)
	if(SEQUENCE){
		res->dfd=open("data/skiplist_data.skip",O_RDWR|O_CREAT|O_TRUNC,0666);
	}
	else{
		res->dfd=open("data/skiplist_data_r.skip",O_RDWR|O_CREAT|O_TRUNC,0666);
	}
	if(res->dfd==-1){
		printf("file open error!\n");
		return NULL;
	}
#endif
#ifdef ENABLE_LIBFTL
	if ( (mio = memio_open() ) == NULL ) {
		printf("memio_open() failed\n");
		return NULL;
	}
#endif
	return res;
}
void buffer_free(buffer *buf){
	free(buf->data);
	for(int i=0; i<LEVELN; i++){
		if(buf->disk[i]!=NULL) level_free(buf->disk[i]);
	}
	if(buf->lastB!=NULL)
		skiplist_meta_free(buf->lastB);
}

void lsm_free(lsmtree *input){
	pthread_mutex_destroy(&sst_lock);
	pthread_mutex_destroy(&mem_lock);

	skiplist_free(input->memtree);
	buffer_free(&input->buf);
#if !defined(ENABLE_LIBFTL)
	close(input->dfd);
#endif
#ifdef ENABLE_LIBFTL
	memio_close(mio);
#endif
	free(input);
}
void lsm_clear(lsmtree *input){
	skiplist_free(input->memtree);
	input->memtree=(skiplist*)malloc(sizeof(skiplist));
	input->memtree=skiplist_init(input->memtree);
}
bool put(lsmtree *LSM,KEYT key, char *value,lsmtree_req_t *req){
	if(key==0)
		printf("key is 0!\n");
	while(1){
		pthread_mutex_lock(&mem_lock);
		if(LSM->memtree->size<KEYN){
			if(value!=NULL)
				skiplist_insert(LSM->memtree,key,value,req,1);
			else
				skiplist_insert(LSM->memtree,key,NULL,req,0);
			pthread_mutex_unlock(&mem_lock);
			return true;
		}
		else{
			pthread_mutex_unlock(&mem_lock);
			continue;
		}
	}
	return false;
}
/*
   MeasureTime mem;
   MeasureTime last;
   MeasureTime buf;
   MeasureTime bp;
   MeasureTime find;
   */
int meta_read_data;
int thread_level_get(lsmtree *LSM,KEYT key, threading *input, char *ret, lsmtree_req_t *req, int l){
	if(l>=LEVELN)
		return 0;
	keyset *temp_key=NULL;

	for(int i=0; i<WAITREQN; i++){
		if(input->pre_req[i]==req){
			input->pre_req[i]=NULL;
			input->entry[i]=NULL;
			break;
		}
	}
	sktable *sk=(sktable*)req->keys;
	temp_key=skiplist_keyset_find(sk,key);
	if(temp_key!=NULL){
		
#ifdef CACHE
		Entry *entry=req->req_entry;
		if(!entry->data){
			entry->data=(sktable*)malloc(sizeof(sktable));
			memcpy(entry->data->meta,req->keys,PAGESIZE);
			cache_print(CH);
			entry->c_entry=cache_insert(CH,entry);
		}
#endif

#ifdef ENABLE_LIBFTL
	memio_free_dma(2,req->dmatag);
#else
	free(req->keys);
#endif
		if(temp_key->ppa==DELETEDKEY){
			printf("[%u]deleted\n",temp_key->key);
			return 6;
		}
		skiplist_keyset_read(temp_key,ret,LSM->dfd,req);
		return 1;
	}

#ifdef ENABLE_LIBFTL
	memio_free_dma(2,req->dmatag);
#else
	free(req->keys);
#endif

	bool metaflag;
	bool returnflag=0;
	for(int i=l+1; i<LEVELN; i++){
		metaflag=false;
		req->flag=i;
		
		Entry *temp=level_find(LSM->buf.disk[i],key);
		if(temp==NULL){
			continue;
		}
#ifdef CACHE
		if(temp->data){
			temp_key=skiplist_keyset_find(temp->data,key);
			if(temp_key!=NULL){
				input->cache_hit++;
				cache_update(CH,temp);
				if(temp_key->ppa==DELETEDKEY){
					printf("[%u]deleted\n",temp_key->key);
					return 6;
				}
				skiplist_keyset_read(temp_key,ret,LSM->dfd,req);
				return 1;
			}
			continue;
		}
#endif
#ifdef BLOOM
		if(!bf_check(temp->filter,key)){
			continue;
		}
#endif	
		input->header_read++;
		req->req_entry=temp;
		skiplist_meta_read_n(temp->pbn,LSM->dfd,0,req);
		return 3;
	}/*
		printf("%d-----------------------------\n",key);
		sktable_print(sk);
		printf("------------------------------\n");*/
	return 0;
}
int thread_get(lsmtree *LSM, KEYT key, threading *input, char *ret,lsmtree_req_t* req){
	req->seq_number=0;
	int tempidx=0;
	snode* res=skiplist_find(LSM->memtree,key);
	if(res!=NULL){
		mem_hit++;
		ret=res->value;
		if(!res->vflag){
			printf("[%u]deleted\n",res->key);
		}
		return 2;
	}

	res=NULL;
	res=skiplist_find(LSM->buf.lastB,key);
	if(res!=NULL){
		mem_hit++;
		ret=res->value;
		if(!res->vflag){
			printf("[%u]deleted\n",res->key);
		}
		return 2;
	}

	int flag=0;
	keyset *temp_key=NULL;
	static int __ppa=0;
	bool metaflag;
	req->type=DISK_READ_T;
	for(int i=0; i<LEVELN; i++){
		metaflag=false;
		req->flag=i;
		
		Entry *temp=level_find(LSM->buf.disk[i],key);
		int return_flag=3;
		if(temp==NULL){
			continue;
		}
#ifdef CACHE
		if(temp->data){
			temp_key=skiplist_keyset_find(temp->data,key);
			if(temp_key!=NULL){
				input->cache_hit++;
				cache_update(CH,temp);
				if(temp_key->ppa==DELETEDKEY){
					printf("[%u]deleted\n",temp_key->key);
					return 6;
				}
				skiplist_keyset_read(temp_key,ret,LSM->dfd,req);
				return 1;
			}
			continue;
		}
#endif

#ifdef BLOOM
		if(!bf_check(temp->filter,key)){
			continue;
		}
#endif
		if(SEQUENCE){
			for(int j=0; j<WAITREQN; j++){
				if(input->pre_req[j]!=NULL){
					if(temp==input->entry[j]){
						return_flag=4;
						pthread_mutex_lock(&input->pre_req[j]->meta_lock);
						sktable *sk=(sktable *)input->pre_req[j]->keys;
						temp_key=skiplist_keyset_find(sk,key);
						pthread_mutex_unlock(&input->pre_req[j]->meta_lock);
						if(temp_key!=NULL){
							if(temp_key->ppa==DELETEDKEY){
								printf("[%u]deleted\n",temp_key->key);
								return 6;
							}
							skiplist_keyset_read(temp_key,ret,LSM->dfd,req);
							pros_hit++;
							return return_flag;
						}
						else{
							metaflag=true;
							break;
						}
					}
				}
			}
		}
		if(SEQUENCE){
			for(int j=0; j<WAITREQN; j++){
				if(input->pre_req[j]==NULL){
					input->pre_req[j]=req;
					input->entry[j]=temp;
					//				input->pre_req[j]->lock_test=1;
					pthread_mutex_lock(&req->meta_lock);
					break;
				}
			}
		}
		input->header_read++;
		req->req_entry=temp;
		if(temp->data){
			printf("???\n");
		}
		skiplist_meta_read_n(temp->pbn,LSM->dfd,0,req);
		return return_flag;
	}
	return 0;
}
lsmtree* lsm_reset(lsmtree* input){
	input->memtree=(skiplist*)malloc(sizeof(skiplist));
	input->memtree=skiplist_init(input->memtree);
	return input;
}
extern int compaction___;
bool compaction(lsmtree *LSM,level *src, level *des,Entry *ent,lsmtree_gc_req_t * req){
	compaction___++;
	static int wn=0;
	KEYT s_start,s_end;
	if(src==NULL){
		s_start=ent->key;
		s_end=ent->end;
	}
	else{
		s_start=src->start;
		s_end=src->end;
	}
	skiplist *last=LSM->buf.lastB;
	if(last==NULL){
		last=(skiplist*)malloc(sizeof(skiplist));
		skiplist_init(last);
	}
	Entry **iter=level_range_find(des,s_start,s_end);

	bool check_getdata=false;
	KEYT *delete_sets=NULL;
	KEYT *delete_pbas=NULL;
	uint8_t **delete_bitset=NULL;
	int deleteIdx=0;
	int allnumber=0;
	req->compt_headers=NULL;
	Entry **entry_sets=NULL;
	if(iter!=NULL && iter[0]!=NULL){
		for(int i=0; iter[i]!=NULL; i++) allnumber++;
		delete_sets=(KEYT*)malloc(sizeof(KEYT)*(des->m_size));
		if(src!=NULL)
			allnumber+=src->size;
		check_getdata=true;
		req->now_number=0;
		req->target_number=0;

		delete_bitset=(uint8_t **)malloc(sizeof(uint8_t*)*(allnumber));
		delete_pbas=(KEYT*)malloc(sizeof(KEYT)*(allnumber));
		req->compt_headers=(sktable*)malloc(sizeof(sktable)*allnumber);
		entry_sets=(Entry **)malloc(sizeof(Entry *)*allnumber);
		int counter=0;
		for(int i=0; iter[i]!=NULL ;i++){
			Entry *temp_e=iter[i];
			entry_sets[counter]=temp_e;
			delete_bitset[counter]=temp_e->bitset;
			delete_sets[deleteIdx++]=temp_e->key;
			delete_pbas[counter]=NODATA;
			temp_e->iscompactioning=true;
/*#ifdef CACHE
			if(!temp_e->data){
#endif*/
				skiplist_meta_read_c(temp_e->pbn,LSM->dfd,counter,req);
				delete_pbas[counter]=temp_e->pbn;
				pthread_mutex_lock(&req->meta_lock);
				req->target_number++;
				pthread_mutex_unlock(&req->meta_lock);
/*#ifdef CACHE
			}
			else{
				cache_delete_entry_only(CH,temp_e);
			}
#endif*/
			counter++;
		}
		if(src!=NULL){
			Iter *level_iter=level_get_Iter(src);
			Entry* iter_temp;
			while((iter_temp=level_get_next(level_iter))!=NULL){
				entry_sets[counter]=iter_temp;
				delete_pbas[counter]=iter_temp->pbn;
				delete_bitset[counter]=iter_temp->bitset;
				iter_temp->iscompactioning=true;
/*#ifdef CACHE
				if(!iter_temp->data){
#endif*/
					skiplist_meta_read_c(iter_temp->pbn,LSM->dfd,counter,req);
					pthread_mutex_lock(&req->meta_lock);
					req->target_number++;
					pthread_mutex_unlock(&req->meta_lock);
/*#ifdef CACHE
				}
				else{
					cache_delete_entry_only(CH,iter_temp);
				}
#endif*/
				counter++;
			}
			free(level_iter);
		}
	}

	if(iter!=NULL)
		free(iter);
	if(!check_getdata){
		if(src==NULL){
			skiplist* temp_skip=(skiplist*)req->data;
//#ifndef CACHE
			ent->pbn=write_data(LSM,(skiplist*)req->data,req,des->fpr);
#ifdef BLOOM
			ent->filter=temp_skip->filter;
#endif
			ent->bitset=temp_skip->bitset;
//#endif
/*#ifdef CACHE
			ent->pbn=0;
			skiplist_data_write(temp_skip,LSM->dfd,req);
			ent->data=skiplist_to_sk(temp_skip,&ent->bitset);
			ent->c_entry=cache_insert(CH,ent);
#endif*/
			skiplist_free(temp_skip);
			level_insert(des,ent);
		}
		else{
#ifndef MONKEY_BLOOM
			Iter *level_iter=level_get_Iter(src);
			Entry *iter_temp;
			while((iter_temp=level_get_next(level_iter))!=NULL){
				Entry *copied_entry=level_entry_copy(iter_temp);
				level_insert(des,copied_entry);
			}
			free(level_iter);
#else
			req->now_number=0;
			req->target_number=0;
			req->compt_headers=(sktable*)malloc(sizeof(sktable)*src->size);
			int counter=0;
			Iter *level_iter=level_get_Iter(src);
			Entry *iter_temp;
			while((iter_temp=level_get_next(level_iter))!=NULL){
/*#ifdef CACHE
				if(!iter_temp->data){
#endif*/
					skiplist_meta_read_c(iter_temp->pbn,LSM->dfd,counter,req);
					pthread_mutex_lock(&req->meta_lock);
					req->target_number++;
					pthread_mutex_unlock(&req->meta_lock);
/*#ifdef CACHE
				}
#endif*/
				counter++;
			}
			free(level_iter);
			lr_gc_req_wait(req);
			level_iter=level_get_Iter(src);
			counter=0;
			while((iter_temp=level_get_next(level_iter))!=NULL){
				Entry* copied_entry=level_entry_copy(iter_temp);
				if(copied_entry->filter){
					bf_free(copied_entry->filter);
					copied_entry->filter=bf_init(KEYN,des->fpr);
				}
/*#ifdef CACHE
				if(!copied_entry->data){
#endif*/
					for(int i=0; i<KEYN; i++){
						bf_set(copied_entry->filter,req->compt_headers[counter].meta[i].key);
					}
/*#ifdef CACHE
				}
#endif*/
				level_insert(des,copied_entry);
				counter++;
			}
			free(level_iter);
#endif
		}
	}
	else{
		lr_gc_req_wait(req);//wait for all read;
		int factor;
		if(src==NULL){
			skiplist* temp_skip=(skiplist*)req->data;
			ent->pbn=skiplist_data_write(temp_skip,LSM->dfd,req);
		}

		skiplist *t;
		snode *temp_s;
		for(int i=0; i<allnumber; i++){
			sktable *sk;
/*#ifdef CACHE
			if(entry_sets[i]->data){
				sk=entry_sets[i]->data;
			}
			else{
#endif*/
				sk=&req->compt_headers[i];
/*#ifdef CACHE
			}
#endif*/
				/* if(!sktable_check(sk)){
				   printf("%d:%ld\n",compaction___,delete_pbas[i]);
				//printf("data error in compaction!\n");
				}*/
			uint8_t *temp_bit=delete_bitset[i];
			for(int k=0; k<KEYN; k++){
				int bit_n=k/8;
				int off=k%8;
				if(sk->meta[k].key==NODATA) continue;
				if(temp_bit[bit_n] & (1<<off)){
					temp_s=skiplist_insert(last,sk->meta[k].key,NULL,NULL,true);
				}
				else{
					temp_s=skiplist_insert(last,sk->meta[k].key,NULL,NULL,false);
				}
				if(temp_s!=NULL)
					temp_s->ppa=sk->meta[k].ppa;
			}
			if(delete_pbas[i]!=NODATA)
				delete_ppa(header_segment,delete_pbas[i]);
		}

		if(src==NULL){
			skiplist* temp_skip=(skiplist*)req->data;
			snode *temp_iter=temp_skip->header->list[1];
			while(temp_iter!=temp_skip->header){
				temp_s=skiplist_insert(last,temp_iter->key,NULL,NULL,temp_iter->vflag);
				if(temp_s!=NULL)
					temp_s->ppa=temp_iter->ppa;
				temp_iter=temp_iter->list[1];
			}
			free_entry(ent);
			skiplist_free(temp_skip);
		}

		for(int i=0; i<deleteIdx; i++){
			level_delete(des,delete_sets[i]);
		}
		while((t=skiplist_cut(last,KEYN > last->size? last->size:KEYN))){
#ifdef M_COMPT
			skiplist_relocate_data(t);
#endif

//#ifndef CACHE
			//no cache
			Entry* temp_e=make_entry(t->start, t->end, write_meta_only(LSM,t,req,des->fpr));
#ifdef BLOOM
			temp_e->filter=t->filter;
#endif
			temp_e->bitset=t->bitset;
			level_insert(des,temp_e);
//#endif

/*#ifdef CACHE
			//using cache
			Entry* temp_e=make_entry(t->start, t->end, 0);
			level_insert(des,temp_e);
			temp_e->data=skiplist_to_sk(t,&temp_e->bitset);
			temp_e->c_entry=cache_insert(CH,temp_e);
#endif*/
			skiplist_meta_free(t);
		}
		free(req->compt_headers);
	}
	if(delete_sets!=NULL){
		free(delete_sets);
	}
	if(delete_pbas!=NULL){
		free(delete_pbas);
	}
	if(delete_bitset!=NULL){
		free(delete_bitset);
	}
	if(entry_sets!=NULL){
		free(entry_sets);
	}
	if(src!=NULL){
		int m_size=src->m_size;
		level **temp_pp;
		for(int i=0; i<LEVELN; i++){
			if(LSM->buf.disk[i]->m_size==m_size){
				temp_pp=&LSM->buf.disk[i];
				break;
			}
		}
		float fpr=src->fpr;
		level_free(src);
		src=(level*)malloc(sizeof(level));
		src=level_init(src,m_size);
		*temp_pp=src;
		src->fpr=fpr;
	}
	LSM->buf.lastB=last;
	return true;
}
bool is_compt_needed(lsmtree *input, KEYT level){
	if(input->buf.disk[level-1]->size<input->buf.disk[level-1]->m_size){
		return false;
	}
	return true;
}
bool is_flush_needed(lsmtree *input){
	pthread_mutex_lock(&mem_lock);
	if(input->memtree->size<KEYN){
		pthread_mutex_unlock(&mem_lock);
		return false;
	}
	else{
		pthread_mutex_unlock(&mem_lock);
		return true;
	}
}

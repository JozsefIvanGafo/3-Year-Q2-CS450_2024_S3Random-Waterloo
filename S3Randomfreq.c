//  10% small Random + 90% main Random + ghost +2-bit counter
//  insert to small random if not in the ghost, else insert to the main random
//  evict from small random:
//      if object in the small is accessed,
//          reinsert to main random,
//      else
//          evict and insert to the ghost
//  evict from main random:
//      if object in the main is accessed,
//          reinsert to main random
//      else
//          evict
//
//
//  S3Random.c
//  libCacheSim
//
//  Created by Juncheng on 12/4/22.
//  Copyright © 2018 Juncheng. All rights reserved.
//

#include "../../dataStructure/hashtable/hashtable.h"
#include "../../include/libCacheSim/evictionAlgo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  cache_t *small_random;
  cache_t *ghost_random;
  cache_t *main_random;
  bool hit_on_ghost;
  int threshold;


  int64_t n_obj_admit_to_small;
  int64_t n_obj_admit_to_main;
  int64_t n_obj_move_to_main;
  int64_t n_byte_admit_to_small;
  int64_t n_byte_admit_to_main;
  int64_t n_byte_move_to_main;

  char main_cache_type[32];

  request_t *req_local;
} S3Randomfreq_params_t;



// ***********************************************************************
// ****                                                               ****
// ****                   function declarations                       ****
// ****                                                               ****
// ***********************************************************************
cache_t *S3Randomfreq_init(const common_cache_params_t ccache_params,
                     const char *cache_specific_params);
static void S3Randomfreq_free(cache_t *cache);
static bool S3Randomfreq_get(cache_t *cache, const request_t *req);

static cache_obj_t *S3Randomfreq_find(cache_t *cache, const request_t *req,
                                const bool update_cache);
static cache_obj_t *S3Randomfreq_insert(cache_t *cache, const request_t *req);
static cache_obj_t *S3Randomfreq_to_evict(cache_t *cache, const request_t *req);
static void S3Randomfreq_evict(cache_t *cache, const request_t *req);
static bool S3Randomfreq_remove(cache_t *cache, const obj_id_t obj_id);
static inline int64_t S3Randomfreq_get_occupied_byte(const cache_t *cache);
static inline int64_t S3Randomfreq_get_n_obj(const cache_t *cache);
static inline bool S3Randomfreq_can_insert(cache_t *cache, const request_t *req);
static void S3Randomfreq_parse_params(cache_t *cache,
                                const char *cache_specific_params);

static void S3Randomfreq_evict_small(cache_t *cache, const request_t *req);
static void S3Randomfreq_evict_main(cache_t *cache, const request_t *req);

// ***********************************************************************
// ****                                                               ****
// ****                   end user facing functions                   ****
// ****                                                               ****
// ***********************************************************************

cache_t *S3Randomfreq_init(const common_cache_params_t ccache_params,
                     const char *cache_specific_params) {
    //We define create the cache structure init
    cache_t *cache =
      cache_struct_init("S3Randomfreq", ccache_params, cache_specific_params);

    //We define the new functions of the cache with the new structure
    cache->cache_init = S3Randomfreq_init;
    cache->cache_free = S3Randomfreq_free;
    cache->get = S3Randomfreq_get;
    cache->find = S3Randomfreq_find;
    cache->insert = S3Randomfreq_insert;
    cache->evict = S3Randomfreq_evict;
    cache->remove = S3Randomfreq_remove;
    cache->to_evict = S3Randomfreq_to_evict;
    cache->get_n_obj = S3Randomfreq_get_n_obj;
    cache->get_occupied_byte = S3Randomfreq_get_occupied_byte;
    cache->can_insert = S3Randomfreq_can_insert;    

    cache->obj_md_size = 0; 


    //We obtain the parameters to later create the size of the cache
    cache->eviction_params = malloc(sizeof(S3Randomfreq_params_t));
    memset(cache->eviction_params, 0, sizeof(S3Randomfreq_params_t));
    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;

    params->req_local = new_request();
    params->hit_on_ghost = false;   
    params->threshold=2;//2 bit counter
    //We parse the parameters 

    //We calculate the size of the caches
    //small size
    int64_t small_size =
        (int64_t)ccache_params.cache_size * 0.1;
    //main size
    int64_t main_cache_size = ccache_params.cache_size - small_size;
    //ghost size
    int64_t ghost_cache_size =
        (int64_t)(ccache_params.cache_size * 0.9); 

    //we create the caches
    common_cache_params_t local_cache_param = ccache_params;
    local_cache_param.cache_size = small_size;
    //create small cache
    params->small_random = Random_init(local_cache_param, NULL);   
    //create ghost cache
    local_cache_param.cache_size= ghost_cache_size;
    params->ghost_random=Random_init(local_cache_param,NULL);
    //create main cache
    local_cache_param.cache_size = main_cache_size;
    params->main_random=Random_init(local_cache_param,NULL);

    //We return cache
    return cache;
}

/**
 * We free the resources the cache is using
 *
 * @param cache
 */
static void S3Randomfreq_free(cache_t *cache) {
    
    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;
    //We free the request
    free_request(params->req_local);

    //we free the != caches used by S3Random
    //free small
    params->small_random->cache_free(params->small_random);
    //free the ghost
    params->ghost_random->cache_free(params->ghost_random);
    //main
    params->main_random->cache_free(params->main_random);

    //We free the eviction parameters
    free(cache->eviction_params);
    //We free the cache structure
    cache_struct_free(cache);
}

/**
 * @brief this function is the user facing API
 * it performs the following logic
 *
 * ```
 * if obj in cache:
 *    update_metadata
 *    return true
 * else:
 *    if cache does not have enough space:
 *        evict until it has space to insert
 *    insert the object
 *    return false
 * ```
 *
 * @param cache
 * @param req
 * @return true if cache hit, false if cache miss
 */
static bool S3Randomfreq_get(cache_t *cache, const request_t *req) {
    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;


    DEBUG_ASSERT(params->small_random->get_occupied_byte(params->small_random)
                    +params->main_random->get_occupied_byte(params->main_random)
                        <= 
                    cache->cache_size);
        

    bool cache_hit = cache_get_base(cache, req);



    return cache_hit;
}

// ***********************************************************************
// ****                                                               ****
// ****       developer facing APIs (used by cache developer)         ****
// ****                                                               ****
// ***********************************************************************
/**
 * @brief find an object in the cache
 *
 * @param cache
 * @param req
 * @param update_cache whether to update the cache,
 *  if true, the object is promoted
 *  and if the object is expired, it is removed from the cache
 * @return the object or NULL if not found
 */
static cache_obj_t *S3Randomfreq_find(cache_t *cache, const request_t *req,
                                const bool update_cache) {

    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;
    //We define the caches to avoid redundancy
    cache_t *small= params->small_random;
    cache_t *main=params->main_random;
    cache_t *ghost=params->ghost_random;

    // if update cache is false, we only check the main and small queues as the ghost doesn't have data
    if (!update_cache) {
        //on small cache??
        cache_obj_t *obj= small->find(small,req, false);
        if (obj != NULL) {
            return obj;
        }
        // on main cache??
        obj= main->find(main,req,false);
        if (obj != NULL) {
            return obj;
        }
        //Not found
        return NULL;
    }
    /* update cache is true from now */
    //we set the hit on ghost is false
    params->hit_on_ghost = false;
    //on small cache???
    cache_obj_t *obj =small->find(small,req,true);
    if (obj != NULL) {
        //We promote from small to main cache
        obj->S3Randomfreq.freq++;
        return obj;
    }
    //on ghost queue???
    //It returns true if the element is inside and is removed from ghost
    if (ghost->remove(ghost,req->obj_id)){
        //We say that is a hit on ghost, but is a miss on the cache
        //so the cache will try to insert the obj and since hit on ghost is true
        //it will be inserted to the main cache
        params->hit_on_ghost = true;
    }
    //on main cache???
    obj=main->find(main,req,true);
    if (obj != NULL){
        obj->S3Randomfreq.freq++;
    }
    return obj;
}

/**
 * @brief insert an object into the cache,
 * update the hash table and cache metadata
 * this function assumes the cache has enough space
 * eviction should be
 * performed before calling this function
 *
 * @param cache
 * @param req
 * @return the inserted object
 */
static cache_obj_t *S3Randomfreq_insert(cache_t *cache, const request_t *req) {

    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;
    //We define the caches  to avoid redundancy
    cache_t *small= params->small_random;
    cache_t *main=params->main_random;

    cache_obj_t *obj = NULL;   
    //if before using this function on find there was a hit then we insert it to main
    if (params->hit_on_ghost) {
        //We deselect the hit on ghost
        params->hit_on_ghost = false;
        // update the counters for the simulator
        params->n_obj_admit_to_main += 1;
        params->n_byte_admit_to_main += req->obj_size;
        //we insert it to main
        //If the object is to big for the main cache then we don't insert it
        if (req->obj_size >= main->cache_size) {
            return NULL;
        }
        obj=main->insert(main,req);
    } 
    //else we insert to the small queue
    else {
      //If the object is to big for the small cache then we don't insert it
      if (req->obj_size >= small->cache_size) {
        return NULL;
      }
      // update the counters for the simulator
      params->n_obj_admit_to_small += 1;
      params->n_byte_admit_to_small += req->obj_size;

      //we insert it to the small cache
      obj= small->insert(small,req);
    }
    obj->S3Randomfreq.freq=0;
    return obj;
}

/**
 * @brief find the object to be evicted
 * this function does not actually evict the object or update metadata
 * not all eviction algorithms support this function
 * because the eviction logic cannot be decoupled from finding eviction
 * candidate, so use assert(false) if you cannot support this function
 *
 * @param cache the cache
 * @return the object to be evicted
 */
static cache_obj_t *S3Randomfreq_to_evict(cache_t *cache, const request_t *req) {
  assert(false);
  return NULL;
}

static void S3Randomfreq_evict_small(cache_t *cache, const request_t *req) {

    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;
    //We define the caches to avoid redundancy
    cache_t *small= params->small_random;
    cache_t *main=params->main_random;
    cache_t *ghost=params->ghost_random;

    //We evict the small cache only if the occupied bytes is bigger than 0
    bool evicted=false;
    while (!evicted && small->get_occupied_byte(small) > 0) {
        // evict from small cache
        cache_obj_t *obj_to_evict =small->to_evict(small,req);

        //we check that there is no empty obj to be evicted
        DEBUG_ASSERT(obj_to_evict != NULL);

        // need to copy the object before it is evicted so that it can be insert it to main if need it
        copy_cache_obj_to_request(params->req_local, obj_to_evict);   
        
        //If object has promoted == true then we promote it to main
        if (obj_to_evict->S3Randomfreq.freq >= params->threshold) {
            // Update statistics
            params->n_obj_move_to_main += 1;
            params->n_byte_move_to_main += obj_to_evict->obj_size;

            //insert it to main
            cache_obj_t *new_obj = main->insert(main, params->req_local);
            new_obj->misc.freq =obj_to_evict->misc.freq;

        } 
        // The obj doesn't have promotion activated so we evict it and save the 
        //pointer on the ghost cache
        else {
            ghost->get(ghost, params->req_local);
            //we evicted
            evicted=true;
        }

    // remove from small cache, but do not update stat
    bool removed = small->remove(small, params->req_local->obj_id);
    assert(removed);
  }
}

static void S3Randomfreq_evict_main(cache_t *cache, const request_t *req) {
    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;
    //We define the caches to avoid redundancy
    cache_t *main=params->main_random;

    // evict from main cache
    //we only evict if the occupied space is bigger than 0
    bool evicted=false;
    while (!evicted && main->get_occupied_byte(main) > 0) {
        //we evict from main

        cache_obj_t *obj_to_evict = main->to_evict(main, req);
        //We check if we evicted the object
        DEBUG_ASSERT(obj_to_evict != NULL);
        int freq =obj_to_evict->S3Randomfreq.freq;



        //We need to do this to create a request to remove the object from the queue
        copy_cache_obj_to_request(params->req_local, obj_to_evict);

        if ( freq >0){
            //main->remove(main,obj_to_evict->obj_id);
            //obj_to_evict = NULL;
            obj_to_evict->S3Randomfreq.freq= MIN(freq,3)-1;
            obj_to_evict->misc.freq=freq;
            //we don't evict it because its frequency is bigger than 0
            //and we reinsert it back
            //cache_obj_t *new_obj = main->insert(main, params->req_local);
            //2 bit counter
            //new_obj->S3Randomfreq.freq = MIN(freq,3)-1;
            //new_obj->misc.freq=freq;


        }else{

            // we remove the object to be evicted 
            bool removed = main->remove(main, obj_to_evict->obj_id);
            // if we cannot remove it then we raise a personalized error
            if (!removed) {
            ERROR("cannot remove obj %ld\n", (long)obj_to_evict->obj_id);
            }   
            evicted=true;
        }
    }
}


/**
 * @brief evict an object from the cache
 * it needs to call cache_evict_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param req not used
 * @param evicted_obj if not NULL, return the evicted object to caller
 */
static void S3Randomfreq_evict(cache_t *cache, const request_t *req) {

    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;
    //We define the caches to avoid redundancy
    cache_t *small= params->small_random;
    cache_t *main=params->main_random;
    cache_t *ghost=params->ghost_random;
    // if the main is full we evict the main cache
    if (main->get_occupied_byte(main) > main->cache_size ||small->get_occupied_byte(small) == 0) {
      return S3Randomfreq_evict_main(cache, req);
    }
    //else we evict the small cache
    return S3Randomfreq_evict_small(cache, req);
}

/**
 * @brief remove an object from the cache
 * this is different from cache_evict because it is used to for user trigger
 * remove, and eviction is used by the cache to make space for new objects
 *
 * it needs to call cache_remove_obj_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param obj_id
 * @return true if the object is removed, false if the object is not in the
 * cache
 */
static bool S3Randomfreq_remove(cache_t *cache, const obj_id_t obj_id) {

    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;
    //We define the caches to avoid redundancy
    cache_t *small= params->small_random;
    cache_t *main=params->main_random;
    cache_t *ghost=params->ghost_random;

    //we remove the objects and if removed successfully it returns a true
    bool removed = false;
    removed = removed || small->remove(small,obj_id) 
                      || ghost->remove(ghost,obj_id)
                      || main->remove(main,obj_id);  
    return removed;
}

static inline int64_t S3Randomfreq_get_occupied_byte(const cache_t *cache) {
    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;
    //We define the caches to avoid redundancy
    cache_t *small= params->small_random;
    cache_t *main=params->main_random;


    return small->get_occupied_byte(small)+main->get_occupied_byte(main);
}

static inline int64_t S3Randomfreq_get_n_obj(const cache_t *cache) {
    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;
    //We define the caches to avoid redundancy
    cache_t *small= params->small_random;
    cache_t *main=params->main_random;
    cache_t *ghost=params->ghost_random;

    return small->get_n_obj(small) +main->get_n_obj(main);
}


static inline bool S3Randomfreq_can_insert(cache_t *cache, const request_t *req) {
    S3Randomfreq_params_t *params = (S3Randomfreq_params_t *)cache->eviction_params;
    //We define the caches to avoid redundancy
    cache_t *small= params->small_random;
    //we only care if we can insert on small
    return req->obj_size <= small->cache_size;
}

#ifdef __cplusplus
}
#endif

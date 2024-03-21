//
//  10% small Random + 90% main Random + ghost
//  insert to small Random if not in the ghost, else insert to the main Random
//  evict from small Random:
//      if object in the small is accessed,
//          reinsert to main Random,
//      else
//          evict and insert to the ghost
//  evict from main Random:
//      if object in the main is accessed,
//          nothing,
//      else
//          evict
//
//
//  S3Random.c
//  libCacheSim
//  Programmed by József Iván Gafo
//
//The structure of our algorithm is similar to the S3FIFO (sosp23-S3FIFO), 
// but instead of using the FIFo policy we are using 
// the Random eviction policy

//Libraries that are need it for the RANDOM policy
#include <stdlib.h> // For rand() and srand()
#include <time.h>   // For seeding srand()

// modules that we may need for our program
#include "../../dataStructure/hashtable/hashtable.h"
#include "../../include/libCacheSim/evictionAlgo.h"

//Allow c++ compatibility
#ifdef __cplusplus
extern "C" {
#endif

//Define the data structure that is need it for the simulator

typedef struct S3Random
{
    //Pointers to the small, main and ghost caches 
    cache_t *small_random;
    cache_t *main_random;
    cache_t *random_ghost;
    //Indicates if there is a hit on cache
    bool hit_on_ghost;

    //Count the number of objects admited on the != caches
    int64_t n_obj_admit_to_small_random;
    int64_t n_obj_admit_to_main_random;
    int64_t n_obj_move_to_main;

    //Count the number of bytes admitted on the != caches
    int64_t n_byte_admit_to_small_random;
    int64_t n_byte_admit_to_main_random;
    int64_t n_byte_move_to_main;

    //Other parameters
    int move_to_main_threshold;
    //ratio of the cache
    double small_random_size_ratio;
    double ghost_size_ratio;

    request_t *req_local;
}S3Random_params_t;

//Default cache parameters
static const char *DEFAULT_CACHE_PARAMS =
    "small-random-size-ratio=0.10,ghost-size-ratio=0.9,move-to-main-threshold=2";



/*

            We declare the functions four our eviction algorithm

*/

// It initializes our  cache algorithm
cache_t *S3Random_init(const common_cache_params_t cache_params,
                        const char *cache_specific_params);

//Free the resources on the S3Random Cache
static void S3Random_free(cache_t *cache);

//lookup a request
static bool S3Random_get(cache_t *cache, const request_t *req);

//it searches for an specific object in the cache and updates the cache metadata
static cache_obj_t *S3Random_find(cache_t *cache, 
                                    const request_t *req, 
                                    const bool update_cache);

//It inserts an object to the cache
static cache_obj_t *S3Random_insert(cache_t *cache, const request_t *req);

//it searches an object on the cache and evicts it
static cache_obj_t *S3Random_to_evict(cache_t *cache, const request_t *req);

//functions that perform the actions

//Performs the eviction process
static void S3Random_evict(cache_t *cache, const request_t *req);

//It removes an element an object from the cache based on its ID (true it has succesfully removed it)
static void S3Random_remove(cache_t *cache, obj_id_t *obj_id);

//Calculate and returns the # of total of occupied bytes in the cache
static inline int64_t S3Random_get_occupied_byte(const cache_t *cache);

//It calculates and returns the total # of bytes stored in the cache
static inline int64_t S3Random_get_n_obj (const cache_t *cache);

//Can we insert a request on the cache
static inline bool S3Random_can_insert(cache_t *cache, const request_t *req);

//function in charge of of parsing and setting the parameters of S3Random cache
static void S3Random_parse_params (cache_t *cache,
                                    const char *cache_specific_params);

//Functions to evict the different queues
static void S3Random_evict_small_random(cache_t *cache, const request_t *req);

static void S3Random_evict_main_random(cache_t *cache, const request_t *req);




/*

    Cache init

*/

cache_t *S3Random_init(const common_cache_params_t ccache_params,
                     const char *cache_specific_params)
    {
    //We create the structure of the cache tha complies with the API
    cache_t *cache=cache_struct_init(
        "S3Random",ccache_params,cache_specific_params
    );
    //We fill all methods and make it compatible with the cache API 
    cache->cache_init=S3Random_init;
    cache->cache_free=S3Random_free;
    cache->get=S3Random_get;
    cache->find=S3Random_find;
    cache->insert=S3Random_insert;
    cache->evict=S3Random_evict;
    cache->remove =S3Random_remove;
    cache->to_evict = S3Random_to_evict;
    cache->get_n_obj = S3Random_get_n_obj;
    cache->get_occupied_byte = S3Random_get_occupied_byte;
    cache->can_insert = S3Random_can_insert;

    cache->obj_md_size = 0;

    //We obtain the eviction parameters
    cache->eviction_params = malloc(sizeof(S3Random_params_t));
    memset(cache->eviction_params, 0, sizeof(S3Random_params_t));
    //Define the params (we convert the cache params into the S3Random_params_t)
    S3Random_params_t *params = (S3Random_params_t *)cache->eviction_params;
    
    params->req_local = new_request();
    params->hit_on_ghost = false;

    //We calculate and obtain the size of each queue of S3Random
    //small random cache size
    int64_t small_random_cache_size=
        (int64_t)(ccache_params.cache_size * params->small_random_size_ratio);
    //main random cache size
    int64_t  main_random_cache_size= 
        ccache_params.cache_size - small_random_cache_size;
    //ghost random cache size
    int64_t ghost_random_cache_size= 
        (int64_t)(ccache_params.cache_size * params->ghost_size_ratio);

    //We create the small cache queue
    //parameters of the small cache queue
    common_cache_params_t ccache_params_local=ccache_params;
    //We cahneg the size to the one we calculated
    ccache_params_local.cache_size=small_random_cache_size;
    params->small_random= Random_init(ccache_params_local, NULL);

    //We create the Ghost cache queue
    //We only create it if the size of the ghost is bigger than 0
    if (ghost_random_cache_size>0)
    {
        //We create the ghost queue
        ccache_params_local.cache_size=ghost_random_cache_size;
        params->random_ghost= Random_init(ccache_params_local,NULL);

    }else{

        params->random_ghost=NULL;
    }

    //We create the Main ghost queue
    ccache_params_local.cache_size=main_random_cache_size;
    params->main_random= Random_init(ccache_params_local,NULL);
    
    //If there is an error add the code for track eviction V age

    return cache;

    }



/**
 * It frees the resources used by the cache
 * Which allows to release the memory not used by the cache
 * 
 * @param cache
*/
static void S3Random_free(cache_t *cache){
     S3Random_params_t *params =
         (S3Random_params_t *)cache->eviction_params;

     //We create a request to free the cache
     free_request(params->req_local);

     //Now we free all the queues of the cache (small, main and/or ghost)
     //We free the small queue
     params->small_random->cache_free(params->req_local);

     //we free the main queue
     params->main_random->cache_free(params->req_local);

     //We free the ghost queue
     if (params->random_ghost!=NULL){
         params->random_ghost->cache_free(params->req_local);
     }
     //We free the memory of the eviction parameters
     free(cache->eviction_params);
     //We free the structure of the cache
     cache_struct_free(cache);
}

/**
 * if obj in cache:
 *   update_metadata (if any)
 *   return True
 * else:
 *   while cache does not have enough space:
 *       evict
 *   insert the object
 *   return False
 *   
 *   @param cache
 *   @param req
 *   @return true if cache hit, else is false (cache miss)
*/
static bool S3Random_get(cache_t *cache, const request_t *req){
    //We convert the cache eviction parameters into the S3Random parameters
    S3Random_params_t *params= (S3Random_params_t *)cache->eviction_params;

    //We make sure that the total number of occupied 
    // bytes on the small queue+ the main queue doesn't exceed the total cache
    DEBUG_ASSERT(
        params->small_random->get_occupied_byte(params->small_random)
        +
        params->main_random->get_occupied_byte(params->main_random)
        <=
        cache->cache_size
    );

    //We see if there is a cache hit (using a function of cache.c)
    bool cache_hit =cache_get_base(cache,req);

    return cache_hit;
}

/**
 * @brief It find an object in the cache
 * 
 * @param cache
 * @param req
 * @param update_cache (true we update the cache)
 * @return it returns the object if found else NULL
*/
static cache_obj_t *S3Random_find(cache_t *cache,
                                const request_t *req,
                                const bool update_cache){

    //We convert the eviction paramaters of cache into the S3random parameters
    S3Random_params_t *params =(S3Random_params_t *)cache->eviction_params;

    //If update cachhe is false we only check if the element 
    //is on the main and/or small random cache
    if (!update_cache){

        //We search if the object requested is on the small queue
        cache_obj_t *obj= params->small_random->find(params->small_random,req,false);

        //If obj!= NULL we found on the small random queue
        if (obj!=NULL){
            return obj;
        }

        //We search on the main random queue
        cache_obj_t *obj = params->main_random->find(params->main_random,req,false);

        //If the obj!= NULL we found our object in our main queue
        if (obj!=NULL){
            return obj;
        }

        //If is we didn't found it on the main or the small queue then is a miss
        return NULL;
    }
    //we need to update the cache

    //We set to false that the last hit was on the ghost queue
    //This will be usefull when we need to promote from the ghost queue to the main queue
    params->hit_on_ghost=false;

    //We find the object on the small queue 
    cache_obj_t *obj= params->small_random->find(params->small_random,req, true);
    if (obj !=NULL){
        //Since we foun the object we promote from the small queue to the main queue
        obj->S3Random.promotion= true;
        return obj;
    }

    //We look on the ghost queue
    //We check if the ghost queue exist and if the object id is found on the ghost queue
    if (params->random_ghost!=NULL && params->random_ghost->remove(params->random_ghost, req->obj_id)){
        //Hit on the ghost queue
        params->hit_on_ghost=true;
    }

    //We retrieve if the object is on the main queue
    obj= params->main_random->find(params->main_random,req,true);
    //We don't need to update the metadata as we don't need it (if it was 2 random then we would need it for the frequency)
    return  obj;
}

/**
 * @brief We insert an element into the cache,
 * It suppose that the cache is free
 * 
 * @param cache
 * @param req
 * @return the inserted object
*/
static cache_obj_t *S3Random_insert(cache_t *cache,
                                    const request_t *req){

    //Convert parameters for S3Random
    S3Random_params_t *params= (S3Random_params_t *)cache->eviction_params;

    //We declare the variable obj with initial value NULL
    //to later return it
    cache_obj_t *obj=NULL;

    //If the erquested object is on the 
    //ghost queue we insert it to the main queue
    if (params->hit_on_ghost){
        params->hit_on_ghost=false;//is no longuer in the ghost queue
        //We update the counters
        params->n_obj_move_to_main+=1;
        params->n_byte_admit_to_main_random+=req->obj_size;
        //We insert the object
        obj=params->main_random->insert(params->main_random,req);
    }

    //Else we insert on the small random queue
    //We check that we have enough space if not we return NULL
    if (req->obj_size>= params->small_random->cache_size){
        return NULL;
    }
    params->n_byte_admit_to_small_random+=req->obj_size;
    params->n_obj_admit_to_small_random+=1;
    obj=params->small_random->insert(params->small_random,req);

    //If error check to add track_eviction_V_AGE and/or track_demotion
    obj->S3Random.promotion=false;
    return obj;
}

/**
 * @brief we find the object to evict, 
 * is not necessary to implement it 
 * 
 * @param cache
 * @return the object to be evicted 
*/
static cache_obj_t *S3Random_to_evict(cache_t *cache, const request_t *req) {
  assert(false);
  return NULL;
}


/**
 * @brief 
 *  if small random occupied bytes >0:
 *         obj= smal random evict
 *         ghost insert obj
 * @param cache cache
 * @param req request
 * 
*/
static void S3Random_evict_small_random(cache_t *cache, const request_t *req){
    
    //Transformation of eviction parameters into S3Random parameters
    S3Random_params_t *params = (S3Random_params_t *)cache->eviction_params;

    cache_t *small= params->small_random;

    if (small->get_occupied_byte(small)>0){

        //We evict an object
        cache_obj_t *obj_to_evict= small->to_evict(small,req);
        //We make sure we evicted (usefull when debugging)
        DEBUG_ASSERT(obj_to_evict!=NULL);

        copy_cache_obj_to_request(params->req_local,obj_to_evict);

        //We added it to the ghost if it exists
        if (params->random_ghost !=NULL){
            params->random_ghost->get(params->random_ghost, params->req_local);

        }
        //We remove it and check if we remove it
        bool removed =small->remove(small,params->req_local->obj_id);
        assert(removed);
    }
}

/**
 * @brief
 * if main random occupied bytes >0
 *      evict main random
 * 
*/
static void S3Random_evict_main_random(cache_t *cache, const request_t *req){
    //Transformation of eviction parameters into S3Random parameters
    S3Random_params_t *params = (S3Random_params_t *)cache->eviction_params;

    cache_t  *main= params->main_random;

    //We only evict if the main is not empty
    if (main->get_occupied_byte(main)>0){
        cache_obj_t *obj_to_evict= main->to_evict(main,req);
        DEBUG_ASSERT(obj_to_evict!=NULL);

        copy_request_to_cache_obj(params->req_local,obj_to_evict);

        bool removed= main->remove(main,params->req_local->obj_id);
        assert(removed);
    }
}

/**
 * @brief 
 * if main random is full or small random empty:
 *      return main random evict
 * else
 *      return small random evict
 * 
 * @param cache
 * @return void
*/
static void S3Random_evict( cache_t *cache, const request_t *req){

    //Transform the eviction parameters into S3RAndom parameters
    S3Random_params_t *params = (S3Random_params_t *)cache->eviction_params;

    cache_t *small=params->small_random;
    cache_t *main= params->main_random;
    cache_t *ghost= params->random_ghost;

    //If the main is full evict or the small queue is empty
    if (main->get_occupied_byte(main)>main->cache_size||
        small->get_occupied_byte(small)==0
    ){
        return S3Random_evict_main_random(cache,req);
    }
    //else we evict the small queue
    return S3Random_evict_small_random(cache,req);    
}




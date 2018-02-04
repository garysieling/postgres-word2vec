#include "postgres.h"
#include "funcapi.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/arrayaccess.h"
#include "stdio.h"
#include "stdlib.h"
#include "time.h"

#include "hashmap.h"

#include "catalog/pg_type.h"

#include "index_utils.h"

typedef struct UsrFctx {
  TopK tk;
  int k;
  int iter;
  char **values;
} UsrFctx;

typedef struct UsrFctxCplx {
  TopKCplx tk;
  int k;
  int iter;
  char **values;
} UsrFctxCplx;

typedef struct UsrFctxBatch {
  TopK* tk;
  int k;
  int iter;
  char **values;
  int* queryIds;
  int queryIdsSize;
} UsrFctxBatch;


typedef struct UsrFctxCluster {
  int* ids;
  int size;
  int* nearestCentroid;
  float** centroids;
  int iter;
  int k; // number of clusters
  char **values;
} UsrFctxCluster;

typedef struct UsrFctxGrouping {
  int* ids;
  int size;
  int* nearestGroup;
  int* groups;
  int iter;
  int groupsSize; // number of groups
  char **values;
} UsrFctxGrouping;

PG_FUNCTION_INFO_V1(pq_search);

Datum
pq_search(PG_FUNCTION_ARGS)
{

  FuncCallContext *funcctx;
  TupleDesc        outtertupdesc;
  TupleTableSlot  *slot;
  AttInMetadata   *attinmeta;
  UsrFctx *usrfctx;

  if (SRF_IS_FIRSTCALL ())
   {
     clock_t start;
     clock_t end;

     Codebook cb;
     int cbPositions = 0;
     int cbCodes = 0;
     float* queryVector;
     int k;
     int subvectorSize;

     float* querySimilarities;

     Datum* queryData;
     int n = 0;

     MemoryContext  oldcontext;

     char *command;
     int ret;
     int proc;
     bool info;

     TopK topK;
     float maxDist;

     char* pqQuantizationTable = palloc(sizeof(char)*100);
     char* pqCodebookTable = palloc(sizeof(char)*100);

     start = clock();

     getTableName(PQ_QUANTIZATION, pqQuantizationTable, 100);
     getTableName(CODEBOOK, pqCodebookTable, 100);

     funcctx = SRF_FIRSTCALL_INIT ();
     oldcontext = MemoryContextSwitchTo (funcctx->multi_call_memory_ctx);

     k = PG_GETARG_INT32(1);

     // get codebook
     cb = getCodebook(&cbPositions, &cbCodes, pqCodebookTable);

     end = clock();
     elog(INFO,"get codebook time %f", (double) (end - start) / CLOCKS_PER_SEC);

    // read query from function args
    getArray(PG_GETARG_ARRAYTYPE_P(0), &queryData, &n);

    queryVector = palloc(n*sizeof(float));
    for (int j=0; j< n; j++){
      queryVector[j] = DatumGetFloat4(queryData[j]);
    }

    subvectorSize = n / cbPositions;

    // determine similarities of codebook entries to query vector
    querySimilarities = palloc(cbPositions*cbCodes*sizeof(float));
    for (int i=0; i< cbPositions*cbCodes; i++){
        int pos = cb[i].pos;
        int code = cb[i].code;
        float* vector = cb[i].vector;
        querySimilarities[pos*cbCodes + code] = squareDistance(queryVector+(pos*subvectorSize), vector, subvectorSize);
    }

    end = clock();
    elog(INFO,"calculate similarities time %f", (double) (end - start) / CLOCKS_PER_SEC);
    // calculate TopK by summing up squared distanced sum method
    topK = palloc(k*sizeof(TopKEntry));
    maxDist = 100.0; // sufficient high value
    for (int i = 0; i < k; i++){
      topK[i].distance = 100.0;
      topK[i].id = -1;
    }

    SPI_connect();
    command = palloc(sizeof(char)*100);
    sprintf(command, "SELECT id, vector FROM %s", pqQuantizationTable);
    ret = SPI_exec(command, 0);
    proc = SPI_processed;
    end = clock();
    elog(INFO,"get quantization data time %f", (double) (end - start) / CLOCKS_PER_SEC);

    if (ret > 0 && SPI_tuptable != NULL){
      TupleDesc tupdesc = SPI_tuptable->tupdesc;
      SPITupleTable *tuptable = SPI_tuptable;

      Oid i_eltype;
      int16 i_typlen;
      bool i_typbyval;
      char i_typalign;
      bool *nulls;

      Datum id;
      Datum vector;
      Datum* data;
      int wordId;
      float distance;

      int code;

      int i;
      for (i = 0; i < proc; i++){

        HeapTuple tuple = tuptable->vals[i];
        id = SPI_getbinval(tuple, tupdesc, 1, &info);
        vector = SPI_getbinval(tuple, tupdesc, 2, &info);
        wordId = DatumGetInt32(id);

        if (i == 0){
        i_eltype = ARR_ELEMTYPE(DatumGetArrayTypeP(vector));
        get_typlenbyvalalign(i_eltype, &i_typlen, &i_typbyval, &i_typalign);
        }

        deconstruct_array(DatumGetArrayTypeP(vector), i_eltype, i_typlen, i_typbyval, i_typalign, &data, &nulls, &n);

        distance = 0;
        for (int j = 0; j < n; j++){
          code = DatumGetInt32(data[j]);
          distance += querySimilarities[j*cbCodes + code];
        }
        if (distance < maxDist){
          updateTopK(topK, distance, wordId, k, maxDist);
          maxDist = topK[k-1].distance;
        }
      }
      SPI_finish();

    }
    end = clock();
    elog(INFO,"calculate distances time %f", (double) (end - start) / CLOCKS_PER_SEC);

    freeCodebook(cb,cbPositions * cbCodes);

    usrfctx = (UsrFctx*) palloc (sizeof (UsrFctx));
    usrfctx -> tk = topK;
    usrfctx -> k = k;
    usrfctx -> iter = 0;
    usrfctx -> values = (char **) palloc (2 * sizeof (char *));
    usrfctx -> values  [0] = (char*) palloc   (16 * sizeof (char));
    usrfctx -> values  [1] = (char*) palloc  (16 * sizeof (char));
    funcctx -> user_fctx = (void *)usrfctx;
    outtertupdesc = CreateTemplateTupleDesc (2 , false);
    TupleDescInitEntry (outtertupdesc,  1, "Id",    INT4OID, -1, 0);
    TupleDescInitEntry (outtertupdesc,  2, "Distance",FLOAT4OID,  -1, 0);
    slot = TupleDescGetSlot (outtertupdesc);
    funcctx -> slot = slot;
    attinmeta = TupleDescGetAttInMetadata (outtertupdesc);
    funcctx -> attinmeta = attinmeta;

    MemoryContextSwitchTo (oldcontext);

    end = clock();
    elog(INFO,"time %f", (double) (end - start) / CLOCKS_PER_SEC);

  }

  funcctx = SRF_PERCALL_SETUP ();
  usrfctx = (UsrFctx*) funcctx -> user_fctx;

  // return results
  if (usrfctx->iter >= usrfctx->k){
      SRF_RETURN_DONE (funcctx);
  }else{

    Datum result;
    HeapTuple outTuple;
    snprintf(usrfctx->values[0], 16, "%d", usrfctx->tk[usrfctx->iter].id);
    snprintf(usrfctx->values[1], 16, "%f", usrfctx->tk[usrfctx->iter].distance);
    usrfctx->iter++;
    outTuple = BuildTupleFromCStrings (funcctx -> attinmeta,
  				      usrfctx -> values);
    result = TupleGetDatum (funcctx -> slot, outTuple);
    SRF_RETURN_NEXT(funcctx, result);

  }

}

PG_FUNCTION_INFO_V1(ivfadc_search);

Datum
ivfadc_search(PG_FUNCTION_ARGS)
{

  FuncCallContext *funcctx;
  TupleDesc        outtertupdesc;
  TupleTableSlot  *slot;
  AttInMetadata   *attinmeta;
  UsrFctx *usrfctx;

  if (SRF_IS_FIRSTCALL ()){

    clock_t start;
    clock_t end;

    MemoryContext  oldcontext;

    Datum* queryData;

    Codebook residualCb;
    int cbPositions = 0;
    int cbCodes = 0;
    int subvectorSize;

    CoarseQuantizer cq;
    int cqSize;

    float* queryVector;
    int k;
    int queryDim;

    float* residualVector;

    int n = 0;

    float* querySimilarities;

    int ret;
    int proc;
    bool info;
    char command[100];

    TopK topK;
    float maxDist;

    // for coarse quantizer
    float minDist; // sufficient high value
    int cqId = -1;

    int foundInstances;
    Blacklist bl;

    char* tableName = palloc(sizeof(char)*100);
    char* tableNameResidualCodebook = palloc(sizeof(char)*100);
    char* tableNameFineQuantization = palloc(sizeof(char)*100);

    start = clock();

    getTableName(NORMALIZED, tableName, 100);
    getTableName(RESIDUAL_CODBOOK, tableNameResidualCodebook, 100);
    getTableName(RESIDUAL_QUANTIZATION, tableNameFineQuantization, 100);

    funcctx = SRF_FIRSTCALL_INIT ();
    oldcontext = MemoryContextSwitchTo (funcctx->multi_call_memory_ctx);

    k = PG_GETARG_INT32(1);


    // get codebook
    residualCb = getCodebook(&cbPositions, &cbCodes, tableNameResidualCodebook);

    // get coarse quantizer
    cq = getCoarseQuantizer(&cqSize);

    end = clock();
    elog(INFO,"get coarse quantizer and residual codebook data time %f", (double) (end - start) / CLOCKS_PER_SEC);

   // read query from function args
   getArray(PG_GETARG_ARRAYTYPE_P(0), &queryData, &n);
   queryVector = palloc(n*sizeof(float));
   queryDim = n;
   for (int j=0; j< queryDim; j++){
     queryVector[j] = DatumGetFloat4(queryData[j]);
   }

   subvectorSize = n / cbPositions;

   foundInstances = 0;
   bl.isValid = false;

   topK = palloc(k*sizeof(TopKEntry));
   maxDist = 100.0; // sufficient high value
   for (int i = 0; i < k; i++){
     topK[i].distance = 100.0;
     topK[i].id = -1;
   }

   while (foundInstances < k){

     Blacklist* newBl;

     // get coarse_quantization(queryVector) (id)
     minDist = 1000;
     for (int i=0; i < cqSize; i++){
       float dist;

       if (inBlacklist(i, &bl)){
         continue;
       }
       dist = squareDistance(queryVector, cq[i].vector, queryDim);
       if (dist < minDist){
         minDist = dist;
         cqId = i;
       }
     }
     end = clock();
     elog(INFO,"determine coarse quantization time %f", (double) (end - start) / CLOCKS_PER_SEC);

     // add coarse quantizer to Blacklist
     newBl = palloc(sizeof(Blacklist));
     newBl->isValid = false;

     addToBlacklist(cqId, &bl, newBl);

     // compute residual = queryVector - coarse_quantization(queryVector)
     residualVector = palloc(queryDim*sizeof(float));
     for (int i = 0; i < queryDim; i++){
       residualVector[i] = queryVector[i] - cq[cqId].vector[i];
     }

     // compute subvector similarities lookup
     // determine similarities of codebook entries to residual vector
     querySimilarities = palloc(cbPositions*cbCodes*sizeof(float));
     for (int i=0; i< cbPositions*cbCodes; i++){
         int pos = residualCb[i].pos;
         int code = residualCb[i].code;
         float* vector = residualCb[i].vector;
         querySimilarities[pos*cbCodes + code] = squareDistance(residualVector+(pos*subvectorSize), vector, subvectorSize);
     }

     end = clock();
     elog(INFO,"precalculate distances time %f", (double) (end - start) / CLOCKS_PER_SEC);

      // calculate TopK by summing up squared distanced sum method

      // connect to databse and compute approximated similarities with sum method
      SPI_connect();
      sprintf(command, "SELECT id, vector FROM %s WHERE coarse_id = %d", tableNameFineQuantization, cq[cqId].id);

      ret = SPI_exec(command, 0);
      proc = SPI_processed;
      end = clock();
      elog(INFO,"get quantization data time %f", (double) (end - start) / CLOCKS_PER_SEC);
      if (ret > 0 && SPI_tuptable != NULL){
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        SPITupleTable *tuptable = SPI_tuptable;
        int i;
        for (i = 0; i < proc; i++){
          Datum id;
          Datum vector;
          Datum* data;
          int wordId;
          float distance;

          HeapTuple tuple = tuptable->vals[i];
          id = SPI_getbinval(tuple, tupdesc, 1, &info);
          vector = SPI_getbinval(tuple, tupdesc, 2, &info);
          wordId = DatumGetInt32(id);
          getArray(DatumGetArrayTypeP(vector), &data, &n);
          distance = 0;
          for (int j = 0; j < n; j++){
            int code = DatumGetInt32(data[j]);
            distance += querySimilarities[j*cbCodes + code];
          }
          if (distance < maxDist){
            updateTopK(topK, distance, wordId, k, maxDist);
            maxDist = topK[k-1].distance;
          }
        }
        SPI_finish();
      }

      foundInstances += proc;

    }

    freeCodebook(residualCb,cbPositions * cbCodes);
    free(cq);

    usrfctx = (UsrFctx*) palloc (sizeof (UsrFctx));
    usrfctx -> tk = topK;
    usrfctx -> k = k;
    usrfctx -> iter = 0;
    usrfctx -> values = (char **) palloc (2 * sizeof (char *));
    usrfctx -> values  [0] = (char*) palloc   (16 * sizeof (char));
    usrfctx -> values  [1] = (char*) palloc  (16 * sizeof (char));
    funcctx -> user_fctx = (void *)usrfctx;
    outtertupdesc = CreateTemplateTupleDesc (2 , false);
    TupleDescInitEntry (outtertupdesc,  1, "Id",    INT4OID, -1, 0);
    TupleDescInitEntry (outtertupdesc,  2, "Distance",FLOAT4OID,  -1, 0);
    slot = TupleDescGetSlot (outtertupdesc);
    funcctx -> slot = slot;
    attinmeta = TupleDescGetAttInMetadata (outtertupdesc);
    funcctx -> attinmeta = attinmeta;

    MemoryContextSwitchTo (oldcontext);

    end = clock();
    elog(INFO,"total time %f", (double) (end - start) / CLOCKS_PER_SEC);

  }
  funcctx = SRF_PERCALL_SETUP ();
  usrfctx = (UsrFctx*) funcctx -> user_fctx;

  // return results
  if (usrfctx->iter >= usrfctx->k){
      SRF_RETURN_DONE (funcctx);
  }else{
    Datum result;
    HeapTuple outTuple;
    snprintf(usrfctx->values[0], 16, "%d", usrfctx->tk[usrfctx->iter].id);
    snprintf(usrfctx->values[1], 16, "%f", usrfctx->tk[usrfctx->iter].distance);
    usrfctx->iter++;
    outTuple = BuildTupleFromCStrings (funcctx -> attinmeta,
                usrfctx -> values);
    result = TupleGetDatum (funcctx -> slot, outTuple);
    SRF_RETURN_NEXT(funcctx, result);

  }
}

PG_FUNCTION_INFO_V1(ivfadc_batch_search);

Datum ivfadc_batch_search(PG_FUNCTION_ARGS){
  FuncCallContext *funcctx;
  TupleDesc        outtertupdesc;
  TupleTableSlot  *slot;
  AttInMetadata   *attinmeta;
  UsrFctxBatch *usrfctx;

  if (SRF_IS_FIRSTCALL ()){

    clock_t start;
    clock_t end;

    MemoryContext  oldcontext;

    Datum* queryData;

    Codebook residualCb;
    int cbPositions = 0;
    int cbCodes = 0;
    int subvectorSize;

    CoarseQuantizer cq;
    int cqSize;

    int* queryIds;
    int queryIdsSize;
    float** queryVectors = NULL;
    int queryVectorsSize = 0;
    int* idArray = NULL;
    int k;
    int queryDim = 0;

    float* residualVector;

    int n = 0;

    float** querySimilarities;

    int ret;
    int proc;
    bool info;
    char* command;
    char* cur;

    TopK* topKs;
    int* maxDists;

    // for coarse quantizer
    float minDist; // sufficient high value
    int* cqIds;
    int** cqTableIds;
    int* cqTableIdCounts;

    int* foundInstances;
    Blacklist* bls;
    bool finished;

    char* tableName = palloc(sizeof(char)*100);
    char* tableNameResidualCodebook = palloc(sizeof(char)*100);
    char* tableNameFineQuantization = palloc(sizeof(char)*100);

    start = clock();

    getTableName(NORMALIZED, tableName, 100);
    getTableName(RESIDUAL_CODBOOK, tableNameResidualCodebook, 100);
    getTableName(RESIDUAL_QUANTIZATION, tableNameFineQuantization, 100);

    funcctx = SRF_FIRSTCALL_INIT ();
    oldcontext = MemoryContextSwitchTo (funcctx->multi_call_memory_ctx);

    k = PG_GETARG_INT32(1);


    // get codebook
    residualCb = getCodebook(&cbPositions, &cbCodes, tableNameResidualCodebook);

    // get coarse quantizer
    cq = getCoarseQuantizer(&cqSize);

    end = clock();
    elog(INFO,"get coarse quantizer data time %f", (double) (end - start) / CLOCKS_PER_SEC);

    // read query from function args
    getArray(PG_GETARG_ARRAYTYPE_P(0), &queryData, &n);
    queryIdsSize = n;
    queryIds = palloc(queryIdsSize*sizeof(int));
    for (int i=0; i< queryIdsSize; i++){
      queryIds[i] = DatumGetInt32(queryData[i]);
    }

    end = clock();
    elog(INFO,"get query ids time %f", (double) (end - start) / CLOCKS_PER_SEC);

    // read out vectors for ids
    SPI_connect();
    command = palloc((100 + queryIdsSize*10)*sizeof(char));
    cur = command;
    cur += sprintf(command, "SELECT id, vector FROM %s WHERE id IN (", tableName);
    for (int i = 0; i < queryIdsSize; i++){
      if (i != (queryIdsSize - 1)){
        cur += sprintf(cur, "%d,", queryIds[i]);
      }else{
        cur += sprintf(cur, "%d)", queryIds[i]);
      }
    }

   ret = SPI_exec(command, 0);
   proc = SPI_processed;
   if (ret > 0 && SPI_tuptable != NULL){
     TupleDesc tupdesc = SPI_tuptable->tupdesc;
     SPITupleTable *tuptable = SPI_tuptable;
     queryVectorsSize = proc;
     queryVectors = malloc(sizeof(float*)*queryVectorsSize);
     idArray = malloc(sizeof(int)*queryVectorsSize);
     for (int i = 0; i < proc; i++){
       Datum id;
       Datum vector;
       Datum* data;

       HeapTuple tuple = tuptable->vals[i];
       id = SPI_getbinval(tuple, tupdesc, 1, &info);
       vector = SPI_getbinval(tuple, tupdesc, 2, &info);
       idArray[i] = DatumGetInt32(id);
       getArray(DatumGetArrayTypeP(vector), &data, &n);
       queryDim = n;
       queryVectors[i] = malloc(queryDim*sizeof(float));
       for (int j = 0; j < queryDim; j++){
         queryVectors[i][j] = DatumGetFloat4(data[j]);
       }
     }
     SPI_finish();
   }

   end = clock();
   elog(INFO,"get vectors for ids time %f", (double) (end - start) / CLOCKS_PER_SEC);

   subvectorSize = queryDim / cbPositions;

   foundInstances = palloc(sizeof(int)*queryVectorsSize);
   topKs = palloc(sizeof(TopK)*queryVectorsSize);
   bls = palloc(sizeof(Blacklist)*queryVectorsSize);
   cqIds = palloc(sizeof(int)*queryVectorsSize);
   querySimilarities = palloc(sizeof(float*)*queryVectorsSize);
   maxDists = palloc(sizeof(int)*queryVectorsSize);
   for (int i = 0; i < queryVectorsSize; i++){
     foundInstances[i] = 0;
     bls[i].isValid = false;
     topKs[i] = palloc(k*sizeof(TopKEntry));
     for (int j = 0; j < k; j++){
       topKs[i][j].distance = 100.0; // sufficient high value
       topKs[i][j].id = -1;
     }
     cqIds[i] = -1;
     maxDists[i] = 100;
   }

   end = clock();
   elog(INFO,"allocate memory time %f", (double) (end - start) / CLOCKS_PER_SEC);

   finished = false;

   while (!finished){
     cqTableIds = palloc(sizeof(int*)*cqSize); // stores all coarseQuantizationIds
     cqTableIdCounts = palloc(sizeof(int)*cqSize);
     for (int i = 0; i < cqSize; i++){
       cqTableIds[i] = NULL;
       cqTableIdCounts[i] = 0;
     }

     finished = true;
     for (int i = 0; i < queryVectorsSize; i++){ // determine coarse quantizations (and residual vectors)
       if (foundInstances[i] >= k){
         continue;
       }

       Blacklist* newBl;

       // get coarse_quantization(queryVector) (id)
       minDist = 1000;
       for (int j=0; j < cqSize; j++){
         float dist;

         if (inBlacklist(j, &(bls[i]))){
           continue;
         }
         dist = squareDistance(queryVectors[i], cq[j].vector, queryDim);
         if (dist < minDist){
           minDist = dist;
           cqIds[i] = j;
         }

       }

       // add coarse quantizer to Blacklist
       newBl = palloc(sizeof(Blacklist));
       newBl->isValid = false;

       addToBlacklist(cqIds[i], &(bls[i]), newBl);
       cqTableIdCounts[cq[cqIds[i]].id] += 1;

       // compute residual = queryVector - coarse_quantization(queryVector)
       residualVector = palloc(queryDim*sizeof(float));
       for (int j = 0; j < queryDim; j++){
         residualVector[j] = queryVectors[i][j] - cq[cqIds[i]].vector[j];
       }
       // compute subvector similarities lookup
       // determine similarities of codebook entries to residual vector
       querySimilarities[i] = palloc(cbPositions*cbCodes*sizeof(float));
       for (int j=0; j< cbPositions*cbCodes; j++){
           int pos = residualCb[j].pos;
           int code = residualCb[j].code;
           float* vector = residualCb[j].vector;
           querySimilarities[i][pos*cbCodes + code] = squareDistance(residualVector+(pos*subvectorSize), vector, subvectorSize);
       }
     }

     end = clock();
     elog(INFO,"precompute distances time %f", (double) (end - start) / CLOCKS_PER_SEC);

     // create cqTableIds lookup -> maps coarse_ids to positions of queryVectors in queryVectors
     for (int i = 0; i < cqSize; i++){
       if (cqTableIdCounts > 0){
         cqTableIds[i] = palloc(sizeof(int)*cqTableIdCounts[i]);
         for (int j = 0; j < cqTableIdCounts[i]; j++){
           cqTableIds[i][j] = 0;
         }
       }
     }
     for (int i = 0; i < queryVectorsSize; i++){
       if (foundInstances[i] >= k){
         continue;
       }
       int j = 0;
       while(cqTableIds[cqIds[i]][j]){
         j++;
       }
       cqTableIds[cqIds[i]][j] = i;
     }

     // determine fine quantization and calculate distances
     SPI_connect();
     command = palloc((200+queryVectorsSize*10)*sizeof(char));
     cur = command;
     cur += sprintf(command, "SELECT id, vector, coarse_id FROM %s WHERE coarse_id IN(", tableNameFineQuantization);
     for (int i = 0; i < queryVectorsSize; i++){
       if (foundInstances[i] >= k){
         continue;
       }
       cur += sprintf(cur, "%d,", cq[cqIds[i]].id);
     }
     sprintf(cur-1, ")");

     end = clock();
     elog(INFO,"create command time %f", (double) (end - start) / CLOCKS_PER_SEC);

     ret = SPI_exec(command, 0);
     proc = SPI_processed;
     end = clock();
     elog(INFO,"get quantization data time %f", (double) (end - start) / CLOCKS_PER_SEC);
     if (ret > 0 && SPI_tuptable != NULL){
       TupleDesc tupdesc = SPI_tuptable->tupdesc;
       SPITupleTable *tuptable = SPI_tuptable;
       for (int i = 0; i < proc; i++){
         Datum id;
         Datum vector;
         Datum coarseIdData;
         Datum* data;
         int wordId;
         int coarseId;
         float distance;

         HeapTuple tuple = tuptable->vals[i];
         id = SPI_getbinval(tuple, tupdesc, 1, &info);
         vector = SPI_getbinval(tuple, tupdesc, 2, &info);
         coarseIdData = SPI_getbinval(tuple, tupdesc, 3, &info);
         wordId = DatumGetInt32(id);
         coarseId = DatumGetInt32(coarseIdData);
         getArray(DatumGetArrayTypeP(vector), &data, &n);
         for (int j = 0; j < cqTableIdCounts[coarseId];j++){
           int queryVectorsIndex = cqTableIds[coarseId][j];
           distance = 0;
           for (int l = 0; l < cbPositions; l++){
             int code = DatumGetInt32(data[l]);
             distance += querySimilarities[queryVectorsIndex][l*cbCodes + code];
           }
           if (distance < maxDists[queryVectorsIndex]){
             updateTopK(topKs[queryVectorsIndex], distance, wordId, k, maxDists[queryVectorsIndex]);
             maxDists[queryVectorsIndex] = topKs[queryVectorsIndex][k-1].distance;
             foundInstances[queryVectorsIndex]++;
           }
         }
       }
       SPI_finish();
     }
     for (int i = 0; i < queryVectorsSize; i++){
       if (foundInstances[i] < k){
         finished = false;
       }
     }

   }

    for (int i = 0; i < queryVectorsSize; i++){
      free(queryVectors[i]);
    }
    free(queryVectors);
    freeCodebook(residualCb,cbPositions * cbCodes);
    free(cq);

    // TODO tokKs ausgeben
    usrfctx = (UsrFctxBatch*) palloc (sizeof (UsrFctxBatch));
    usrfctx -> tk = topKs;
    usrfctx -> k = k;
    usrfctx -> queryIds = idArray;
    usrfctx -> queryIdsSize = queryVectorsSize;
    usrfctx -> iter = 0;
    usrfctx -> values = (char **) palloc (3 * sizeof (char *));
    usrfctx -> values  [0] = (char*) palloc   (16 * sizeof (char));
    usrfctx -> values  [1] = (char*) palloc   (16 * sizeof (char));
    usrfctx -> values  [2] = (char*) palloc  (16 * sizeof (char));
    funcctx -> user_fctx = (void *)usrfctx;
    outtertupdesc = CreateTemplateTupleDesc (3 , false);

    TupleDescInitEntry (outtertupdesc,  1, "QueryId",    INT4OID, -1, 0);
    TupleDescInitEntry (outtertupdesc,  2, "Id",    INT4OID, -1, 0);
    TupleDescInitEntry (outtertupdesc,  3, "Distance",FLOAT4OID,  -1, 0);
    slot = TupleDescGetSlot (outtertupdesc);
    funcctx -> slot = slot;
    attinmeta = TupleDescGetAttInMetadata (outtertupdesc);
    funcctx -> attinmeta = attinmeta;

    MemoryContextSwitchTo (oldcontext);

    end = clock();
    elog(INFO,"total time %f", (double) (end - start) / CLOCKS_PER_SEC);

  }
  funcctx = SRF_PERCALL_SETUP ();
  usrfctx = (UsrFctxBatch*) funcctx -> user_fctx;

  // return results
  if (usrfctx->iter >= usrfctx->k * usrfctx->queryIdsSize){
      SRF_RETURN_DONE (funcctx);
      free(usrfctx -> queryIds);
      elog(INFO, "deleted it");
  }else{
    Datum result;
    HeapTuple outTuple;
    snprintf(usrfctx->values[0], 16, "%d", usrfctx->queryIds[usrfctx->iter / usrfctx->k]);
    snprintf(usrfctx->values[1], 16, "%d", usrfctx->tk[usrfctx->iter / usrfctx->k][usrfctx->iter % usrfctx->k].id);
    snprintf(usrfctx->values[2], 16, "%f", usrfctx->tk[usrfctx->iter / usrfctx->k][usrfctx->iter % usrfctx->k].distance);
    usrfctx->iter++;
    outTuple = BuildTupleFromCStrings (funcctx -> attinmeta,
                usrfctx -> values);
    result = TupleGetDatum (funcctx -> slot, outTuple);
    SRF_RETURN_NEXT(funcctx, result);

  }
}

PG_FUNCTION_INFO_V1(pq_search_in);

Datum
pq_search_in(PG_FUNCTION_ARGS)
{

  FuncCallContext *funcctx;
  TupleDesc        outtertupdesc;
  TupleTableSlot  *slot;
  AttInMetadata   *attinmeta;
  UsrFctx *usrfctx;

  if (SRF_IS_FIRSTCALL ())
   {


    int k;

    Datum* queryData;
    float* queryVector;
    int queryVectorSize;

    Datum* idsData;
    int* inputIds;
    int inputIdSize;

    int n = 0;

    Codebook cb;
    int cbPositions = 0;
    int cbCodes = 0;
    int subvectorSize;

    float* querySimilarities;
    TopK topK;
    float maxDist;

    int ret;
    int proc;
    bool info;
    char* command;
    char * cur;

    MemoryContext  oldcontext;

    char* tableNameCodebook = palloc(sizeof(char)*100);
    char* tableNamePqQuantization = palloc(sizeof(char)*100);

    getTableName(CODEBOOK, tableNameCodebook, 100);
    getTableName(PQ_QUANTIZATION, tableNamePqQuantization, 100);


    funcctx = SRF_FIRSTCALL_INIT ();
    oldcontext = MemoryContextSwitchTo (funcctx->multi_call_memory_ctx);

    k = PG_GETARG_INT32(1);

    // read query from function args
    getArray(PG_GETARG_ARRAYTYPE_P(0), &queryData, &n);
    queryVector = palloc(n*sizeof(float));
    for (int j=0; j< n; j++){
      queryVector[j] = DatumGetFloat4(queryData[j]);
    }
    queryVectorSize = n;

    // read ids from function args
    getArray(PG_GETARG_ARRAYTYPE_P(2), &idsData, &n);
    inputIds = palloc(n*sizeof(float));
    for (int j=0; j< n; j++){
      inputIds[j] = DatumGetInt32(idsData[j]);
    }
    inputIdSize = n;

    // get pq codebook
    cb = getCodebook(&cbPositions, &cbCodes, tableNameCodebook);

    subvectorSize = queryVectorSize / cbPositions;

    // determine similarities of codebook entries to query vector
    querySimilarities = palloc(cbPositions*cbCodes*sizeof(float));
    for (int i=0; i< cbPositions*cbCodes; i++){
      int pos = cb[i].pos;
      int code = cb[i].code;
      float* vector = cb[i].vector;
      querySimilarities[pos*cbCodes + code] = squareDistance(queryVector+(pos*subvectorSize), vector, subvectorSize);
    }

    // calculate TopK by summing up squared distanced sum method
    topK = palloc(k*sizeof(TopKEntry));
    maxDist = 100.0; // sufficient high value
    for (int i = 0; i < k; i++){
      topK[i].distance = 100.0;
      topK[i].id = -1;
    }
    // get codes for all entries with an id in inputIds -> SQL Query
    SPI_connect();
    command = palloc(60* sizeof(char) + inputIdSize*10*sizeof(char));
    sprintf(command, "SELECT id, vector FROM %s WHERE id IN (", tableNamePqQuantization);
    // fill command
    cur = command + strlen(command);
    for (int i = 0; i < inputIdSize; i++){
      if ( i == inputIdSize - 1){
          cur += sprintf(cur, "%d", inputIds[i]);
      }else{
        cur += sprintf(cur, "%d, ", inputIds[i]);
      }
    }
    cur += sprintf(cur, ")");

    ret = SPI_exec(command, 0);
    proc = SPI_processed;
    if (ret > 0 && SPI_tuptable != NULL){
      TupleDesc tupdesc = SPI_tuptable->tupdesc;
      SPITupleTable *tuptable = SPI_tuptable;
      int i;
      for (i = 0; i < proc; i++){
        Datum id;
        Datum vector;
        Datum* data;
        int wordId;
        float distance;

        HeapTuple tuple = tuptable->vals[i];
        id = SPI_getbinval(tuple, tupdesc, 1, &info);
        vector = SPI_getbinval(tuple, tupdesc, 2, &info);
        wordId = DatumGetInt32(id);
        getArray(DatumGetArrayTypeP(vector), &data, &n);
        distance = 0;
        for (int j = 0; j < n; j++){
          int code = DatumGetInt32(data[j]);
          distance += querySimilarities[j*cbCodes + code];
        }
        if (distance < maxDist){
          updateTopK(topK, distance, wordId, k, maxDist);
          maxDist = topK[k-1].distance;
        }
      }
      SPI_finish();

      usrfctx = (UsrFctx*) palloc (sizeof (UsrFctx));
      usrfctx -> tk = topK;
      usrfctx -> k = k;
      usrfctx -> iter = 0;
      usrfctx -> values = (char **) palloc (2 * sizeof (char *));
      usrfctx -> values  [0] = (char*) palloc   (16 * sizeof (char));
      usrfctx -> values  [1] = (char*) palloc  (16 * sizeof (char));
      funcctx -> user_fctx = (void *)usrfctx;
      outtertupdesc = CreateTemplateTupleDesc (2 , false);
      TupleDescInitEntry (outtertupdesc,  1, "Id",    INT4OID, -1, 0);
      TupleDescInitEntry (outtertupdesc,  2, "Distance",FLOAT4OID,  -1, 0);
      slot = TupleDescGetSlot (outtertupdesc);
      funcctx -> slot = slot;
      attinmeta = TupleDescGetAttInMetadata (outtertupdesc);
      funcctx -> attinmeta = attinmeta;

      MemoryContextSwitchTo (oldcontext);

    }

    freeCodebook(cb,cbPositions * cbCodes);
  }
  funcctx = SRF_PERCALL_SETUP ();
  usrfctx = (UsrFctx*) funcctx -> user_fctx;

  // return results
  if (usrfctx->iter >= usrfctx->k){
      SRF_RETURN_DONE (funcctx);
  }else{

    Datum result;
    HeapTuple outTuple;
    snprintf(usrfctx->values[0], 16, "%d", usrfctx->tk[usrfctx->iter].id);
    snprintf(usrfctx->values[1], 16, "%f", usrfctx->tk[usrfctx->iter].distance);
    usrfctx->iter++;
    outTuple = BuildTupleFromCStrings (funcctx -> attinmeta,
  				      usrfctx -> values);
    result = TupleGetDatum (funcctx -> slot, outTuple);
    SRF_RETURN_NEXT(funcctx, result);

  }

}

int free_hashmap_entry(any_t item, any_t data){
  free((char*) item);
  free((float*) data);
  return MAP_OK;
}

PG_FUNCTION_INFO_V1(pq_search_in_cplx);

Datum
pq_search_in_cplx(PG_FUNCTION_ARGS)
{

  FuncCallContext *funcctx;
  TupleDesc        outtertupdesc;
  TupleTableSlot  *slot;
  AttInMetadata   *attinmeta;
  UsrFctxCplx *usrfctx;

  if (SRF_IS_FIRSTCALL ())
   {

    int k;

    Datum* queryData;
    float* queryVector;
    int queryVectorSize;

    Datum* termsData;
    char*** inputTerms;
    int inputTermsSize;
    int inputTermsPlaneSize;

    int n = 0;

    Codebook cb;
    int cbPositions = 0;
    int cbCodes = 0;
    int subvectorSize;

    float* querySimilarities;
    TopKCplx topK;
    float maxDist;

    int ret;
    int proc;
    bool info;
    char* command;
    char * cur;

    map_t mymap;
    int ecode;

    MemoryContext  oldcontext;

    char* tableNameCodebook = palloc(sizeof(char)*100);
    char* tableNamePqQuantization = palloc(sizeof(char)*100);

    mymap = hashmap_new();

    getTableName(CODEBOOK, tableNameCodebook, 100);
    getTableName(PQ_QUANTIZATION, tableNamePqQuantization, 100);

    funcctx = SRF_FIRSTCALL_INIT ();
    oldcontext = MemoryContextSwitchTo (funcctx->multi_call_memory_ctx);

    k = PG_GETARG_INT32(1);

    // read query from function args
    getArray(PG_GETARG_ARRAYTYPE_P(0), &queryData, &n);
    queryVector = palloc(n*sizeof(float));
    for (int j=0; j< n; j++){
      queryVector[j] = DatumGetFloat4(queryData[j]);
    }
    queryVectorSize = n;

    // read string array and transform it to 2-dimensional# token array
    getArray(PG_GETARG_ARRAYTYPE_P(2), &termsData, &n);
    inputTerms = palloc(n*sizeof(char**));
    inputTermsPlaneSize = 0;
    for (int j=0; j< n; j++){
      char* term = palloc(sizeof(char)*(VARSIZE(termsData[j]) - VARHDRSZ+1));
      snprintf(term, VARSIZE(termsData[j]) + 1 - VARHDRSZ, "%s",(char*) VARDATA(termsData[j]));
      inputTermsPlaneSize += strlen(term);
      inputTerms[j] = split(term, ' ');
    }
    inputTermsSize = n;

    // get pq codebook
    cb = getCodebook(&cbPositions, &cbCodes, tableNameCodebook);

    subvectorSize = queryVectorSize / cbPositions;

    // determine similarities of codebook entries to query vector
    querySimilarities = palloc(cbPositions*cbCodes*sizeof(float));
    for (int i=0; i< cbPositions*cbCodes; i++){
      int pos = cb[i].pos;
      int code = cb[i].code;
      float* vector = cb[i].vector;
      querySimilarities[pos*cbCodes + code] = squareDistance(queryVector+(pos*subvectorSize), vector, subvectorSize);
    }

    // get codes for all entries with an id in inputTerms -> SQL Query
    SPI_connect();
    command = palloc(60* sizeof(char) + inputTermsPlaneSize*5*sizeof(char)); // TODO determine maximal string size more precise
    sprintf(command, "SELECT word, vector FROM %s WHERE word IN (", tableNamePqQuantization);
    // fill command
    cur = command + strlen(command);
    for (int i = 0; i < inputTermsSize; i++){
      for (int j = 0; inputTerms[i][j]; j++){
        if (( i == inputTermsSize - 1) && (inputTerms[i][j+1] == NULL)){
            cur += sprintf(cur, "'%s'", inputTerms[i][j]);
        }else{
          cur += sprintf(cur, "'%s', ", inputTerms[i][j]);
        }
      }
    }
    cur += sprintf(cur, ")");
    ret = SPI_exec(command, 0);
    proc = SPI_processed;
    if (ret > 0 && SPI_tuptable != NULL){
      TupleDesc tupdesc = SPI_tuptable->tupdesc;
      SPITupleTable *tuptable = SPI_tuptable;
      int i;
      for (i = 0; i < proc; i++){
        char* word;
        Datum vector;
        Datum* data;
        char* wordText = malloc(sizeof(char)* 100);
        float* distancePointer = malloc(sizeof(float));

        HeapTuple tuple = tuptable->vals[i];
        word = SPI_getvalue(tuple, tupdesc, 1);
        vector = SPI_getbinval(tuple, tupdesc, 2, &info);
        snprintf(wordText, strlen(word)+1, "%s", word);
        getArray(DatumGetArrayTypeP(vector), &data, &n);
        *distancePointer = 0;
        for (int j = 0; j < n; j++){
          int code = DatumGetInt32(data[j]);
          *distancePointer += querySimilarities[j*cbCodes + code];
        }
        // put distance in hashmap
        ecode = hashmap_put(mymap, wordText,distancePointer);
      }
      SPI_finish();
      // calculate TopK
      topK = palloc(k*sizeof(TopKWordEntry));
      maxDist = 100.0; // sufficient high value
      for (int i = 0; i < k; i++){
        topK[i].distance = 100.0;
        topK[i].word = palloc(100*sizeof(char));
      }
      for (int i = 0; i < inputTermsSize; i++){
        float minDist = 100;
        for (int j = 0; inputTerms[i][j]; j++){
          float* currentDist;
          ecode = hashmap_get(mymap, inputTerms[i][j], (void**)&currentDist);
          if ((ecode == MAP_OK) && (*currentDist < minDist)){
            minDist = *currentDist;
          }
        }
        if (minDist < maxDist){
          updateTopKCplx(topK, minDist, inputTerms[i], k, maxDist);
          maxDist = topK[k-1].distance;
        }
      }

      // free hashmap
      hashmap_iterate(mymap, free_hashmap_entry);
      hashmap_free(mymap);


      for (int i = 0; i < inputTermsSize; i++){
        for (int j = 0; inputTerms[i][j]; j++){
          free(inputTerms[i][j]);
        }
        free(inputTerms[i]);
      }

      usrfctx = (UsrFctxCplx*) palloc (sizeof (UsrFctxCplx));
      usrfctx -> tk = topK;
      usrfctx -> k = k;
      usrfctx -> iter = 0;
      usrfctx -> values = (char **) palloc (2 * sizeof (char *));
      usrfctx -> values  [0] = (char*) palloc   (100 * sizeof (char));
      usrfctx -> values  [1] = (char*) palloc  (16 * sizeof (char));
      funcctx -> user_fctx = (void *)usrfctx;
      outtertupdesc = CreateTemplateTupleDesc (2 , false);
      TupleDescInitEntry (outtertupdesc,  1, "Term",    VARCHAROID, -1, 0);
      TupleDescInitEntry (outtertupdesc,  2, "Distance",FLOAT4OID,  -1, 0);
      slot = TupleDescGetSlot (outtertupdesc);
      funcctx -> slot = slot;
      attinmeta = TupleDescGetAttInMetadata (outtertupdesc);
      funcctx -> attinmeta = attinmeta;

      MemoryContextSwitchTo (oldcontext);

    }

    freeCodebook(cb,cbPositions * cbCodes);
  }
  funcctx = SRF_PERCALL_SETUP ();
  usrfctx = (UsrFctxCplx*) funcctx -> user_fctx;

  // return results
  if (usrfctx->iter >= usrfctx->k){
      SRF_RETURN_DONE (funcctx);
  }else{

    Datum result;
    HeapTuple outTuple;
    snprintf(usrfctx->values[0], 100, "%s", usrfctx->tk[usrfctx->iter].word);
    snprintf(usrfctx->values[1], 16, "%f", usrfctx->tk[usrfctx->iter].distance);
    usrfctx->iter++;
    outTuple = BuildTupleFromCStrings (funcctx -> attinmeta,
  				      usrfctx -> values);
    result = TupleGetDatum (funcctx -> slot, outTuple);
    SRF_RETURN_NEXT(funcctx, result);

  }

}

PG_FUNCTION_INFO_V1(cluster_pq);

Datum
cluster_pq(PG_FUNCTION_ARGS)
{
  // input: array of ids to cluster, number of clusters
  // output: set of cluster vectors -> arrays of ids corresponding to cluster vectors

  FuncCallContext *funcctx;
  TupleDesc        outtertupdesc;
  TupleTableSlot  *slot;
  AttInMetadata   *attinmeta;
  UsrFctxCluster *usrfctx;
  int vectorSize = 300; // TODO change this

  const int DATASET_SIZE = 3000000; // TODO get this dynamically

  if (SRF_IS_FIRSTCALL ()){

    int n = 0;

    MemoryContext  oldcontext;

    Datum* idsData;

    int* inputIds;
    int inputIdsSize;

    int* kmCentroidIds;

    int k;
    int iterations;

    float** querySimilarities;

    Codebook cb;
    int cbPositions = 0;
    int cbCodes = 0;
    int subvectorSize;

    // data structure for relation id -> nearest centroid
    int* nearestCentroid;

    // store number of nearest vectors per centroid
    int* relationCounts;

    // randomly choosen init vectors for centroids
    WordVectors idVectors;

    // centroids
    float** kmCentroids;

    // unnormalized new centroids
    float** kmCentroidsNew;

    char* tableName;
    char* tableNameCodebook = palloc(sizeof(char)*100);
    char* tableNamePqQuantization = palloc(sizeof(char)*100);

    getTableName(CODEBOOK, tableNameCodebook, 100);
    getTableName(PQ_QUANTIZATION, tableNamePqQuantization, 100);

    funcctx = SRF_FIRSTCALL_INIT ();
    oldcontext = MemoryContextSwitchTo (funcctx->multi_call_memory_ctx);

    k = PG_GETARG_INT32(1);

    iterations = 10;

    relationCounts = palloc(sizeof(int)*k);

    nearestCentroid = palloc(sizeof(int)*(DATASET_SIZE+1));

    // read ids from function args
    getArray(PG_GETARG_ARRAYTYPE_P(0), &idsData, &n);

    inputIds = palloc(n*sizeof(int));
    for (int j=0; j< n; j++){
      inputIds[j] = DatumGetInt32(idsData[j]);
    }
    inputIdsSize = n;

    if (inputIdsSize < k){
      elog(ERROR, "|ids| < k");
      SRF_RETURN_DONE (funcctx);
    }

    // get pq codebook
    cb = getCodebook(&cbPositions, &cbCodes, tableNameCodebook);

    subvectorSize = vectorSize / cbPositions;

    // choose initial km-centroid randomly
    kmCentroidIds = palloc(sizeof(int)*k);
    shuffle(inputIds, kmCentroidIds, inputIdsSize, k);

    // get vectors for ids
    tableName = palloc(sizeof(char)*100);

    getTableName(NORMALIZED, tableName, 100);
    idVectors = getVectors(tableName, kmCentroidIds, k);
    kmCentroids = idVectors.vectors;

    kmCentroidsNew = palloc(sizeof(float*)*k);
    for (int i = 0; i < k; i++){
      kmCentroidsNew[i] = palloc(sizeof(float)*vectorSize);
    }


    for (int iteration = 0; iteration < iterations; iteration++){

      int ret;
      int proc;
      bool info;
      char* command;
      char * cur;

      // init kmCentroidsNew
      for (int i=0; i<k;i++){
        for (int j = 0; j < vectorSize; j++){
          kmCentroidsNew[i][j] = 0;
        }
      }

      // determine similarities of codebook entries to km_centroid vector
      querySimilarities = palloc(sizeof(float*) * k);

      for (int cs = 0; cs < k; cs++){
        querySimilarities[cs] = palloc(cbPositions*cbCodes*sizeof(float));
        for (int i=0; i< cbPositions*cbCodes; i++){
          int pos = cb[i].pos;
          int code = cb[i].code;
          float* vector = cb[i].vector;
          querySimilarities[cs][pos*cbCodes + code] = squareDistance(kmCentroids[cs]+(pos*subvectorSize), vector, subvectorSize);
        }
      }


      // reset counts for relation
      for (int i = 0; i < k; i++){
        relationCounts[i] = 0;
      }

      // get vectors for ids
      // get codes for all entries with an id in inputIds -> SQL Query
      SPI_connect();

      command = palloc(200* sizeof(char) + inputIdsSize*8*sizeof(char));
      sprintf(command, "SELECT pq_quantization.id, pq_quantization.vector, vecs.vector FROM %s AS pq_quantization INNER JOIN %s AS vecs ON vecs.id = pq_quantization.id WHERE pq_quantization.id IN (", tableNamePqQuantization, tableName);
      // fill command
      cur = command + strlen(command);
      for (int i = 0; i < inputIdsSize; i++){
        if ( i == inputIdsSize - 1){
            cur += sprintf(cur, "%d", inputIds[i]);
        }else{
          cur += sprintf(cur, "%d, ", inputIds[i]);
        }
      }
      cur += sprintf(cur, ")");

      ret = SPI_exec(command, 0);
      proc = SPI_processed;
      if (ret > 0 && SPI_tuptable != NULL){
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        SPITupleTable *tuptable = SPI_tuptable;
        int i;
        for (i = 0; i < proc; i++){
          Datum id;
          Datum pqVector;
          Datum bigVector;

          Datum* dataPqVector;
          Datum* dataBigVector;

          int wordId;
          float distance;
          int pqSize;

          // variables to determine best match
          float minDist = 100; // sufficient high value

          HeapTuple tuple = tuptable->vals[i];
          id = SPI_getbinval(tuple, tupdesc, 1, &info);
          pqVector = SPI_getbinval(tuple, tupdesc, 2, &info);
          bigVector = SPI_getbinval(tuple, tupdesc, 3, &info);

          wordId = DatumGetInt32(id);

          getArray(DatumGetArrayTypeP(pqVector), &dataPqVector, &n);
          pqSize = n;

          getArray(DatumGetArrayTypeP(bigVector), &dataBigVector, &n);



          for (int centroidIndex = 0; centroidIndex < k; centroidIndex++){
            distance = 0;
            for (int j = 0; j < pqSize; j++){
              int code = DatumGetInt32(dataPqVector[j]);
              distance += querySimilarities[centroidIndex][j*cbCodes + code];
            }

            if (distance < minDist){
              minDist = distance;
              nearestCentroid[wordId] = centroidIndex;
            }
          }
          relationCounts[nearestCentroid[wordId]]++;
          for (int j = 0; j < vectorSize; j++){
            kmCentroidsNew[nearestCentroid[wordId]][j] += DatumGetFloat4(dataBigVector[j]);
          }
        }
        SPI_finish();
      }
      // calculate new km-centroids
      for (int cs = 0; cs < k; cs++){
        for (int pos = 0; pos < vectorSize; pos++){
          if (relationCounts[cs] > 0){
            kmCentroids[cs][pos] = kmCentroidsNew[cs][pos] / relationCounts[cs];
          }else{
            kmCentroids[cs][pos] = 0;
          }
          kmCentroidsNew[cs][pos] = kmCentroids[cs][pos];
        }
      }
    }

    freeWordVectors(idVectors, k);

    usrfctx = (UsrFctxCluster*) palloc (sizeof (UsrFctxCluster));
    usrfctx -> ids = inputIds;
    usrfctx -> size = inputIdsSize;
    usrfctx -> nearestCentroid = nearestCentroid;
    usrfctx -> centroids = kmCentroidsNew;
    usrfctx -> iter = 0;
    usrfctx -> k = k;

    usrfctx -> values = (char **) palloc (2 * sizeof (char *));
    usrfctx -> values  [0] = (char*) palloc   ((18 * vectorSize + 4) * sizeof (char));
    usrfctx -> values  [1] = (char*) palloc  ((inputIdsSize * 8) * sizeof (char));
    funcctx -> user_fctx = (void *)usrfctx;
    outtertupdesc = CreateTemplateTupleDesc (2 , false);
    TupleDescInitEntry (outtertupdesc,  1, "Vector",    FLOAT4ARRAYOID, -1, 0);
    TupleDescInitEntry (outtertupdesc,  2, "Ids",INT4ARRAYOID,  -1, 0);
    slot = TupleDescGetSlot (outtertupdesc);
    funcctx -> slot = slot;
    attinmeta = TupleDescGetAttInMetadata (outtertupdesc);
    funcctx -> attinmeta = attinmeta;

    MemoryContextSwitchTo (oldcontext);

  }
  funcctx = SRF_PERCALL_SETUP ();
  usrfctx = (UsrFctxCluster*) funcctx -> user_fctx;

 // return results
  if (usrfctx->iter >= usrfctx->k){
    SRF_RETURN_DONE (funcctx);
  }else{

    Datum result;
    HeapTuple outTuple;
    char* cursor;

    // construct output values[0] -> cluster vector; values[1] -> id array
    sprintf(usrfctx->values[0], "{ ");
    cursor = usrfctx->values[0] + strlen("{ ");
    for (int i = 0; i < vectorSize; i++){
      if (i < vectorSize - 1 ){
        cursor += sprintf(cursor, "%f, ", usrfctx->centroids[usrfctx->iter][i]);
      }else{
        sprintf(cursor, "%f}", usrfctx->centroids[usrfctx->iter][i]);
      }
    }

    sprintf(usrfctx->values[1], "{ ");
    cursor = usrfctx->values[1] + strlen("{ ");
    for (int i = 0; i < usrfctx->size; i++){
      if (usrfctx->nearestCentroid[usrfctx->ids[i]] == usrfctx->iter){
      cursor += sprintf(cursor, " %d,", usrfctx->ids[i]);
      }
    }
    sprintf(cursor-1, "}");

    usrfctx->iter++;
    outTuple = BuildTupleFromCStrings (funcctx -> attinmeta,
              usrfctx -> values);
    result = TupleGetDatum (funcctx -> slot, outTuple);
    SRF_RETURN_NEXT(funcctx, result);

  }
}

PG_FUNCTION_INFO_V1(grouping_pq);

Datum
grouping_pq(PG_FUNCTION_ARGS)
{

  FuncCallContext *funcctx;
  TupleDesc        outtertupdesc;
  TupleTableSlot  *slot;
  AttInMetadata   *attinmeta;
  UsrFctxGrouping *usrfctx;
  int vectorSize = 300;

  if (SRF_IS_FIRSTCALL ()){

    int n = 0;

    MemoryContext  oldcontext;

    Datum* idsData;
    Datum* groupIdData;

    int* groupIds;
    int groupIdsSize;

    int* inputIds;
    int inputIdsSize;

    float** groupVecs;
    int groupVecsSize;

    float** querySimilarities;

    Codebook cb;
    int cbPositions = 0;
    int cbCodes = 0;
    int subVectorSize;

    int* nearestGroup;

    int ret;
    int proc;
    bool info;
    char* command;
    char * cur;

    char* tableName = palloc(sizeof(char)*100);
    char* tableNameCodebook = palloc(sizeof(char)*100);
    char* tableNamePqQuantization = palloc(sizeof(char)*100);

    getTableName(NORMALIZED, tableName, 100);
    getTableName(CODEBOOK, tableNameCodebook, 100);
    getTableName(PQ_QUANTIZATION, tableNamePqQuantization, 100);

    funcctx = SRF_FIRSTCALL_INIT ();
    oldcontext = MemoryContextSwitchTo (funcctx->multi_call_memory_ctx);

    // read ids from function args

    getArray(PG_GETARG_ARRAYTYPE_P(0), &idsData, &n);
    inputIds = palloc(n*sizeof(int));
    for (int j=0; j< n; j++){
      inputIds[j] = DatumGetInt32(idsData[j]);
    }
    inputIdsSize = n;

    // read group ids from function args
    getArray(PG_GETARG_ARRAYTYPE_P(1), &groupIdData, &n);
    groupIds = palloc(n*sizeof(int));
    for (int j=0; j< n; j++){
      groupIds[j] = DatumGetInt32(groupIdData[j]);
    }
    groupIdsSize = n;
    qsort(groupIds, groupIdsSize, sizeof(int), compare);

    // read group vectors
    SPI_connect();
    command = palloc( 100* sizeof(char) + inputIdsSize*10*sizeof(char));
    sprintf(command, "SELECT id, vector FROM %s WHERE id IN (", tableName);
    // fill command
    cur = command + strlen(command);
    for (int i = 0; i < groupIdsSize; i++){
      if ( i == groupIdsSize - 1){
          cur += sprintf(cur, "%d", groupIds[i]);
      }else{
        cur += sprintf(cur, "%d, ", groupIds[i]);
      }
    }
    cur += sprintf(cur, ") ORDER BY id ASC");

    groupVecs = malloc(sizeof(float*)*n);
    groupVecsSize = n;

    ret = SPI_exec(command, 0);
    proc = SPI_processed;
    if (proc != groupIdsSize){
      elog(ERROR, "Group ids do not exist");
    }
    if (ret > 0 && SPI_tuptable != NULL){
      TupleDesc tupdesc = SPI_tuptable->tupdesc;
      SPITupleTable *tuptable = SPI_tuptable;
      for (int i = 0; i < proc; i++){
        Datum groupVector;
        Datum* dataGroupVector;
        HeapTuple tuple = tuptable->vals[i];

        groupVector = SPI_getbinval(tuple, tupdesc, 2, &info);
        getArray(DatumGetArrayTypeP(groupVector), &dataGroupVector, &n);
        vectorSize = n; // one asignment would be enough...
        groupVecs[i] = malloc(sizeof(float)*vectorSize);
        for (int j = 0; j < vectorSize; j++){
          groupVecs[i][j] = DatumGetFloat4(dataGroupVector[j]);
        }
      }
    }
    SPI_finish();

    nearestGroup = palloc(sizeof(int)*(inputIdsSize));

    // get pq codebook
    cb = getCodebook(&cbPositions, &cbCodes, tableNameCodebook);

    subVectorSize = vectorSize / cbPositions;

    querySimilarities = palloc(sizeof(float*) * groupVecsSize);

    for (int cs = 0; cs < groupVecsSize; cs++){
      querySimilarities[cs] = palloc(cbPositions*cbCodes*sizeof(float));
      for (int i=0; i< cbPositions*cbCodes; i++){
        int pos = cb[i].pos;
        int code = cb[i].code;
        float* vector = cb[i].vector;
        querySimilarities[cs][pos*cbCodes + code] = squareDistance(groupVecs[cs]+(pos*subVectorSize), vector, subVectorSize);
      }
    }
    // get vectors for group_ids
    // get codes for all entries with an id in inputIds -> SQL Query
    SPI_connect();
    command = palloc(200* sizeof(char) + inputIdsSize*8*sizeof(char));
    sprintf(command, "SELECT pq_quantization.id, pq_quantization.vector FROM %s AS pq_quantization WHERE pq_quantization.id IN (", tableNamePqQuantization);
    // fill command
    cur = command + strlen(command);
    for (int i = 0; i < inputIdsSize; i++){
      if ( i == inputIdsSize - 1){
          cur += sprintf(cur, "%d", inputIds[i]);
      }else{
        cur += sprintf(cur, "%d, ", inputIds[i]);
      }
    }
    cur += sprintf(cur, ")");

    ret = SPI_exec(command, 0);
    proc = SPI_processed;
    inputIdsSize = proc;
    if (ret > 0 && SPI_tuptable != NULL){
      TupleDesc tupdesc = SPI_tuptable->tupdesc;
      SPITupleTable *tuptable = SPI_tuptable;
      int i;
      for (i = 0; i < proc; i++){
        Datum id;
        Datum pqVector;

        Datum* dataPqVector;

        float distance;
        int pqSize;

        // variables to determine best match
        float minDist = 100; // sufficient high value

        HeapTuple tuple = tuptable->vals[i];
        id = SPI_getbinval(tuple, tupdesc, 1, &info);
        pqVector = SPI_getbinval(tuple, tupdesc, 2, &info);

        inputIds[i] = DatumGetInt32(id);
        getArray(DatumGetArrayTypeP(pqVector), &dataPqVector, &n);
        pqSize = n;


        for (int groupIndex = 0; groupIndex < groupVecsSize; groupIndex++){
          distance = 0;
          for (int j = 0; j < pqSize; j++){
            int code = DatumGetInt32(dataPqVector[j]);
            distance += querySimilarities[groupIndex][j*cbCodes + code];
          }

          if (distance < minDist){
            minDist = distance;
            nearestGroup[i] = groupIndex;

          }
        }
      }
      SPI_finish();
    }

    for (int i = 0; i < groupVecsSize; i++){
      free(groupVecs[i]);
    }
    free(groupVecs);

    usrfctx = (UsrFctxGrouping*) palloc (sizeof (UsrFctxGrouping));
    usrfctx -> ids = inputIds;
    usrfctx -> size = inputIdsSize;
    usrfctx -> nearestGroup = nearestGroup;
    usrfctx -> groups = groupIds;
    usrfctx -> iter = 0;
    usrfctx -> groupsSize = groupIdsSize;
    usrfctx -> values = (char **) palloc (2 * sizeof (char *));
    usrfctx -> values  [0] = (char*) palloc  ((18) * sizeof (char));
    usrfctx -> values  [1] = (char*) palloc   ((18 * vectorSize + 4) * sizeof (char));
    funcctx -> user_fctx = (void *)usrfctx;
    outtertupdesc = CreateTemplateTupleDesc (2 , false);
    TupleDescInitEntry (outtertupdesc,  1, "Ids",    INT4OID, -1, 0);
    TupleDescInitEntry (outtertupdesc,  2, "GroupIds",INT4OID,  -1, 0);
    slot = TupleDescGetSlot (outtertupdesc);
    funcctx -> slot = slot;
    attinmeta = TupleDescGetAttInMetadata (outtertupdesc);
    funcctx -> attinmeta = attinmeta;

    MemoryContextSwitchTo (oldcontext);

  }
  funcctx = SRF_PERCALL_SETUP ();
  usrfctx = (UsrFctxGrouping*) funcctx -> user_fctx;

 // return results
  if (usrfctx->iter >= usrfctx->size){
    SRF_RETURN_DONE (funcctx);
  }else{

    Datum result;
    HeapTuple outTuple;

    sprintf(usrfctx->values[0], "%d", usrfctx->ids[usrfctx->iter]);
    sprintf(usrfctx->values[1], "%d", usrfctx -> groups[usrfctx->nearestGroup[usrfctx->iter]]);

    usrfctx->iter++;
    outTuple = BuildTupleFromCStrings (funcctx -> attinmeta,
              usrfctx -> values);
    result = TupleGetDatum (funcctx -> slot, outTuple);
    SRF_RETURN_NEXT(funcctx, result);

  }
}


PG_FUNCTION_INFO_V1(insert_batch);

Datum
insert_batch(PG_FUNCTION_ARGS)
{

  int n;

  Datum* termsData;
  int inputTermsSize;
  int inputTermsPlaneSize;
  char** inputTerms;

  int ret;
  int proc;
  bool info;
  char* command;
  char* cur;

  float** rawVectors;
  float** rawVectorsUnnormalized;
  int vectorSize = 0;
  char** tokens;
  int rawVectorsSize;

  CodebookWithCounts cb;
  int cbPositions = 0;
  int cbCodes = 0;
  int subvectorSize;

  int** nearestCentroids;
  int* countIncs;

  int* cqQuantizations;
  float* nearestCoarseCentroidRaw;
  float** residuals;

  CodebookWithCounts residualCb;
  int cbrPositions = 0;
  int cbrCodes = 0;
  int residualSubvectorSize;
  CoarseQuantizer cq;
  int cqSize;
  int** nearestResidualCentroids;
  int* residualCountIncs;
  float minDistCoarse;


  char* tableNameCodebook = palloc(sizeof(char)*100);
  char* pqQuantizationTable = palloc(sizeof(char)*100);
  char* tableNameResidualCodebook = palloc(sizeof(char)*100);
  char* tableNameFineQuantization = palloc(sizeof(char)*100);

  char* tableNameNormalized = palloc(sizeof(char)*100);
  char* tableNameOriginal = palloc(sizeof(char)*100);

  getTableName(CODEBOOK, tableNameCodebook, 100);
  getTableName(PQ_QUANTIZATION, pqQuantizationTable, 100);

  getTableName(RESIDUAL_CODBOOK, tableNameResidualCodebook, 100);
  getTableName(RESIDUAL_QUANTIZATION, tableNameFineQuantization, 100);

  getTableName(NORMALIZED, tableNameNormalized, 100);
  getTableName(ORIGINAL, tableNameOriginal, 100);

  // get terms from arguments
  getArray(PG_GETARG_ARRAYTYPE_P(0), &termsData, &n);
  inputTerms = palloc(n*sizeof(char*));
  inputTermsPlaneSize = 0;
  for (int j=0; j< n; j++){
    char* term = palloc(sizeof(char)*(VARSIZE(termsData[j]) - VARHDRSZ+1));
    snprintf(term, VARSIZE(termsData[j]) + 1 - VARHDRSZ, "%s",(char*) VARDATA(termsData[j]));
    inputTermsPlaneSize += strlen(term);
    inputTerms[j] = term;
  }
  inputTermsSize = n;

  // determine tokenization
  command = palloc(sizeof(char)*inputTermsPlaneSize*3 + 100);
  cur = command;
  cur += sprintf(cur, "SELECT replace(term, ' ', '_') AS token, tokenize(term), tokenize_raw(term) FROM unnest('{");
  for (int i=0; i < inputTermsSize; i++){
    if (i < (inputTermsSize-1)){
      cur += sprintf(cur, "%s, ", inputTerms[i]);
    }else{
      cur += sprintf(cur, "%s", inputTerms[i]);;
    }
  }
  cur += sprintf(cur, "}'::varchar(100)[]) AS term");
  SPI_connect();
  ret = SPI_exec(command, 0);
  proc = SPI_processed;
  rawVectorsSize = proc;
  rawVectors = malloc(sizeof(float*)*proc);
  rawVectorsUnnormalized = malloc(sizeof(float*)*proc);
  tokens = malloc(sizeof(char*)*proc);
  if (ret > 0 && SPI_tuptable != NULL){
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;
    for (int i = 0; i < proc; i++){
      Datum vector;
      Datum vectorUnnormalized;
      char* token;

      Datum* dataVector;
      Datum* dataVectorUnnormlized;

      HeapTuple tuple = tuptable->vals[i];

      token = SPI_getvalue(tuple, tupdesc, 1);
      vector = SPI_getbinval(tuple, tupdesc, 2, &info);
      vectorUnnormalized = SPI_getbinval(tuple, tupdesc, 3, &info);
      tokens[i] = malloc(sizeof(char)* 100); // maybe replace with strlen(token)+1 when using TEXT data type

      snprintf(tokens[i], strlen(token)+1, "%s", token);
      getArray(DatumGetArrayTypeP(vector), &dataVector, &n);
      getArray(DatumGetArrayTypeP(vectorUnnormalized), &dataVectorUnnormlized, &n);
      vectorSize = n;
      rawVectors[i] = malloc(sizeof(float)*vectorSize);
      rawVectorsUnnormalized[i] = malloc(sizeof(float)*vectorSize);
      for (int j= 0; j < vectorSize; j++){
        rawVectors[i][j] = DatumGetFloat4(dataVector[j]);
        rawVectorsUnnormalized[i][j] = DatumGetFloat4(dataVectorUnnormlized[j]);
      }
    }
    SPI_finish();
  }
  pfree(command);

  // determine quantization and count increments

  // get pq codebook
  cb = getCodebookWithCounts(&cbPositions, &cbCodes, tableNameCodebook);

  // get residual codebook
  residualCb = getCodebookWithCounts(&cbrPositions, &cbrCodes, tableNameResidualCodebook);

  // get coarse quantizer
  cq = getCoarseQuantizer(&cqSize);

  // determine coarse quantization and residuals
  cqQuantizations = palloc(sizeof(int)*rawVectorsSize);
  nearestCoarseCentroidRaw = NULL;
  residuals = malloc(sizeof(float*)*rawVectorsSize);

  for (int i = 0; i < rawVectorsSize; i++){
    minDistCoarse = 100;
    for (int j = 0; j < cqSize; j++){
      float dist = squareDistance(rawVectors[i], cq[j].vector, vectorSize);
      if (dist < minDistCoarse){
        cqQuantizations[i] = cq[j].id;
        nearestCoarseCentroidRaw = cq[j].vector;
        minDistCoarse = dist;
      }
    }
    residuals[i] = malloc(sizeof(float)*vectorSize);
    for (int j = 0; j < vectorSize; j++){
      residuals[i][j] = rawVectors[i][j] - nearestCoarseCentroidRaw[j];
    }
  }

  // determine nearest centroids (quantization)
  nearestCentroids = palloc(sizeof(int*)*rawVectorsSize);
  countIncs = malloc(cbPositions*cbCodes*sizeof(int));
  subvectorSize = vectorSize / cbPositions;

  updateCodebook(rawVectors, rawVectorsSize, subvectorSize, cb, cbPositions, cbCodes, nearestCentroids, countIncs);

  nearestResidualCentroids = palloc(sizeof(int*)*rawVectorsSize);
  residualSubvectorSize = vectorSize / cbrPositions;
  residualCountIncs = malloc(cbrPositions*cbrCodes*sizeof(int));
  updateCodebook(residuals, rawVectorsSize, residualSubvectorSize, residualCb, cbrPositions, cbrCodes, nearestResidualCentroids, residualCountIncs);

  // insert new terms + quantinzation
  updateProductQuantizationRelation(nearestCentroids, tokens, cbPositions, cb, pqQuantizationTable, rawVectorsSize, NULL);
  // insert new terms + quantinzation for residuals
  updateProductQuantizationRelation(nearestResidualCentroids, tokens, cbrPositions, residualCb, tableNameFineQuantization, rawVectorsSize, cqQuantizations);

  // update codebook relation
  updateCodebookRelation(cb, cbPositions, cbCodes, tableNameCodebook, countIncs, subvectorSize);
  // update residual codebook relation
  updateCodebookRelation(residualCb, cbrPositions, cbrCodes, tableNameResidualCodebook, residualCountIncs, residualSubvectorSize);

  updateWordVectorsRelation(tableNameNormalized, tokens, rawVectors, rawVectorsSize, vectorSize);
  updateWordVectorsRelation(tableNameOriginal, tokens, rawVectorsUnnormalized, rawVectorsSize, vectorSize);

  for (int i = 0; i < rawVectorsSize; i++){
      free(rawVectors[i]);
      free(rawVectorsUnnormalized[i]);
      free(tokens[i]);
      free(residuals[i]);
  }
  free(rawVectors);
  free(rawVectorsUnnormalized);
  free(tokens);
  free(countIncs);
  freeCodebookWithCounts(cb,cbPositions*cbCodes);

  freeCodebookWithCounts(residualCb, cbrPositions*cbrCodes);
  free(cq);
  free(residualCountIncs);
  free(residuals);

  PG_RETURN_INT32(0);

}
#include "index_utils.h"

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "stdlib.h"
#include "time.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/array.h"

void updateTopK(TopK tk, float distance, int id, int k, int maxDist, int bestPos){
  int i;
  for (i = k-1; i >= bestPos; i--){
    if (tk[i].distance < distance){
      break;
    }
  }
  i++;
  for (int j = k-2; j >= i; j--){
    tk[j+1].distance = tk[j].distance;
    tk[j+1].id = tk[j].id;
  }
  tk[i].distance = distance;
  tk[i].id = id;
}

void updateTopKWordEntry(char** term, char* word){
  char* cur = word;
  memset(word,0,strlen(word));
  for (int p = 0; term[p]; p++){
    if (term[p+1] == NULL){
      cur += sprintf(cur, "%s", term[p]);
    }else{
      cur += sprintf(cur, "%s ", term[p]);
    }
  }
}

void updateTopKCplx(TopKCplx tk, float distance, char** term, int k, int maxDist, int bestPos){
  int i;
  for (i = k-1; i >= bestPos; i--){
    if (tk[i].distance < distance){
      break;
    }
  }
  i++;
  for (int j = k-2; j >= i; j--){
    tk[j+1].distance = tk[j].distance;
    strcpy(tk[j+1].word, tk[j].word);
  }
  tk[i].distance = distance;
  updateTopKWordEntry(term, tk[i].word);
}


bool inBlacklist(int id, Blacklist* bl){
  if (bl->isValid){
    if (bl->id == id){
      return true;
    }else{
      return inBlacklist(id, bl->next);
    }
  }else{
    return false;
  }
}

void addToBlacklist(int id, Blacklist* bl, Blacklist* emptyBl){
  while (bl->isValid){
    bl = bl->next;
  }
  bl->id = id;
  bl->next = emptyBl;
  bl->isValid = true;
}

float squareDistance(float* v1, float* v2, int n){
  float result = 0;
  for (int i = 0; i < n; i++){
    float prod = (v1[i] - v2[i])*(v1[i] - v2[i]);
    result += prod;
  }

  return result;
}

void shuffle(int* input, int* output, int inputSize, int outputSize){
  int i;
  int j;
  int t;
  int* tmp = malloc(sizeof(int)*inputSize);
  srand(time(0));

  for (i = 0; i < inputSize; i++){
    tmp[i] = input[i];
  }
  for (i = 0; i < outputSize; i++)
  {
    j = i + rand() / (RAND_MAX / (inputSize - i) + 1);
    t = tmp[j];
    tmp[j] = tmp[i];
    tmp[i] = t;
  }
  for (i = 0; i < outputSize; i++){
    output[i] = tmp[i];
  }
  free(tmp);
}

CoarseQuantizer getCoarseQuantizer(int* size){
  char* command;
  int ret;
  int proc;
  CoarseQuantizer result;

  char* tableNameCQ = palloc(sizeof(char)*100);
  getTableName(COARSE_QUANTIZATION, tableNameCQ, 100);

  SPI_connect();
  command = palloc(100*sizeof(char));
  sprintf(command, "SELECT * FROM %s", tableNameCQ);
  ret = SPI_exec(command, 0);
  proc = SPI_processed;
  *size = proc;
  result = malloc(proc * sizeof(CoarseQuantizerEntry));
  if (ret > 0 && SPI_tuptable != NULL){
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;
    int i;
    for (i = 0; i < proc; i++){
      Datum id;
      Datum vector;
      Datum* data;

      Oid eltype;
      int16 typlen;
      bool typbyval;
      char typalign;
      bool *nulls;
      int n = 0;

      ArrayType* vectorAt;

      bool info;
      HeapTuple tuple = tuptable->vals[i];
      id = SPI_getbinval(tuple, tupdesc, 1, &info);
      vector = SPI_getbinval(tuple, tupdesc, 2, &info);
      vectorAt = DatumGetArrayTypeP(vector);
      eltype = ARR_ELEMTYPE(vectorAt);
      get_typlenbyvalalign(eltype, &typlen, &typbyval, &typalign);
      deconstruct_array(vectorAt, eltype, typlen, typbyval, typalign, &data, &nulls, &n);

      result[i].id = DatumGetInt32(id);
      result[i].vector = malloc(n*sizeof(float));
      for (int j=0; j< n; j++){
        result[i].vector[j] = DatumGetFloat4(data[j]);
      }
    }
    SPI_finish();
  }


  return result;
}

Codebook getCodebook(int* positions, int* codesize, char* tableName){
  char command[50];
  int ret;
  int proc;
  Codebook result;
  SPI_connect();
  sprintf(command, "SELECT * FROM %s", tableName);

  ret = SPI_exec(command, 0);
  proc = SPI_processed;
  result = malloc(proc * sizeof(CodebookEntry));
  if (ret > 0 && SPI_tuptable != NULL){
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;
    int i;
    for (i = 0; i < proc; i++){

      Datum pos;
      Datum code;
      Datum vector;
      Datum* data;

      Oid eltype;
      int16 typlen;
      bool typbyval;
      char typalign;
      bool *nulls;
      int n = 0;

      ArrayType* vectorAt;

      bool info;
      HeapTuple tuple = tuptable->vals[i];
      pos = SPI_getbinval(tuple, tupdesc, 2, &info);
      code = SPI_getbinval(tuple, tupdesc, 3, &info);
      vector = SPI_getbinval(tuple, tupdesc, 4, &info);
      vectorAt = DatumGetArrayTypeP(vector);
      eltype = ARR_ELEMTYPE(vectorAt);
      get_typlenbyvalalign(eltype, &typlen, &typbyval, &typalign);
      deconstruct_array(vectorAt, eltype, typlen, typbyval, typalign, &data, &nulls, &n);

      (*positions) = fmax((*positions), pos);
      (*codesize) = fmax((*codesize), code);

      result[i].pos = DatumGetInt32(pos);
      result[i].code = DatumGetInt32(code);
      result[i].vector = malloc(n*sizeof(float));
      for (int j=0; j< n; j++){
        result[i].vector[j] = DatumGetFloat4(data[j]);
      }
    }
    SPI_finish();
  }

  *positions += 1;
  *codesize += 1;

  return result;
}

void freeCodebook(Codebook cb, int size){
  for (int i = 0; i < size; i++){
    free(cb[i].vector);
  }
  free(cb);
}

WordVectors getVectors(char* tableName, int* ids, int idsSize){
  char* command;
  char* cur;
  int ret;
  int proc;
  bool info;

  Oid eltype;
  int16 typlen;
  bool typbyval;
  char typalign;
  bool *nulls;
  int n = 0;

  WordVectors result;

  result.vectors = malloc(sizeof(float*)*idsSize);
  result.ids = malloc(sizeof(int)*idsSize);

  SPI_connect();

  command = palloc((100 + idsSize*18)*sizeof(char));
  sprintf(command, "SELECT id, vector FROM %s WHERE id IN (", tableName);
  // fill command
  cur = command + strlen(command);
  for (int i = 0; i < idsSize; i++){
    if ( i == idsSize - 1){
        cur += sprintf(cur, "%d", ids[i]);
    }else{
      cur += sprintf(cur, "%d, ", ids[i]);
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
      ArrayType* vectorAt;
      int wordId;

      HeapTuple tuple = tuptable->vals[i];
      id = SPI_getbinval(tuple, tupdesc, 1, &info);
      vector = SPI_getbinval(tuple, tupdesc, 2, &info);
      wordId = DatumGetInt32(id);
      vectorAt = DatumGetArrayTypeP(vector);
      eltype = ARR_ELEMTYPE(vectorAt);
      get_typlenbyvalalign(eltype, &typlen, &typbyval, &typalign);
      deconstruct_array(vectorAt, eltype, typlen, typbyval, typalign, &data, &nulls, &n);
      result.vectors[i] = malloc(sizeof(float)*n);
      result.ids[i] = wordId;
      for (int j = 0; j < n; j++){
        result.vectors[i][j] = DatumGetFloat4(data[j]);
      }
    }
  }
  SPI_finish();

  return result;
}

void freeWordVectors(WordVectors vectors, int size){
  for (int i = 0; i < size; i++){
    free(vectors.vectors[i]);
  }
  free(vectors.vectors);
  free(vectors.ids);
}

void getArray(ArrayType* input, Datum** result, int* n){
  Oid i_eltype;
  int16 i_typlen;
  bool i_typbyval;
  char i_typalign;
  bool *nulls;

  i_eltype = ARR_ELEMTYPE(input);
  get_typlenbyvalalign(i_eltype, &i_typlen, &i_typbyval, &i_typalign);
  deconstruct_array(input, i_eltype, i_typlen, i_typbyval, i_typalign, result, &nulls, n);
}

void getTableName(tableType type, char* name, int bufferSize){
  char* command;
  int ret;
  int proc;

  const char* function_names[] = {
    "get_vecs_name_original()",
    "get_vecs_name()",
    "get_vecs_name_pq_quantization()",
    "get_vecs_name_codebook()",
    "get_vecs_name_residual_quantization()",
    "get_vecs_name_coarse_quantization()",
    "get_vecs_name_residual_codebook()"};

  SPI_connect();

  command = palloc(100*sizeof(char));
  sprintf(command, "SELECT * FROM %s", function_names[type]);

  ret = SPI_exec(command, 0);
  proc = SPI_processed;
  if (ret > 0 && SPI_tuptable != NULL){
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;
    HeapTuple tuple;
    if (proc != 1){
      elog(ERROR, "Unexpected number of results: %d", proc);
    }
    tuple = tuptable->vals[0];

    snprintf(name, bufferSize, "%s", SPI_getvalue(tuple, tupdesc, 1));
  }
  SPI_finish();
}

// inspired by https://stackoverflow.com/questions/9210528/split-string-with-delimiters-in-c?answertab=oldest#tab-top
typedef struct {
    const char *start;
    size_t len;
} token;

char **split(const char *str, char sep)
{
    char **array;
    unsigned int start = 0, stop, toks = 0, t;
    token *tokens = malloc((strlen(str) + 1) * sizeof(token));
    for (stop = 0; str[stop]; stop++) {
        if (str[stop] == sep) {
            tokens[toks].start = str + start;
            tokens[toks].len = stop - start;
            toks++;
            start = stop + 1;
        }
    }
    /* Mop up the last token */
    tokens[toks].start = str + start;
    tokens[toks].len = stop - start;
    toks++;
    array = malloc((toks + 1) * sizeof(char*));
    for (t = 0; t < toks; t++) {
        /* Calloc makes it nul-terminated */
        char *token = calloc(tokens[t].len + 1, 1);
        memcpy(token, tokens[t].start, tokens[t].len);
        array[t] = token;
    }
    /* Add a sentinel */
    array[t] = NULL;
    free(tokens);
    return array;
}

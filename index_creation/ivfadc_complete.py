#!/bin/python3

from scipy.cluster.vq import kmeans
from scipy.spatial.distance import sqeuclidean
from scipy.spatial.distance import cdist
import sys
import numpy as np
import faiss
import time
import psycopg2
import pickle

from config import *
from logger import *
from vector_feeder import *
from ivfadc_index_creator import *
import index_utils as utils
import index_manager as im
import quantizer_creation as qcreator
import database_export as db_export


USE_PIPELINE_APPROACH = True
USE_BYTEA_TYPE = True

def get_table_information(index_config):
    if USE_BYTEA_TYPE:
        return ((index_config.get_value('coarse_table_name'),"(id serial PRIMARY KEY, vector bytea, count int)"),
            (index_config.get_value('fine_table_name'),"(id serial PRIMARY KEY, coarse_tag integer, word_id integer, vector bytea)"),
            (index_config.get_value('cb_table_name'), "(id serial PRIMARY KEY, pos int, code int, vector bytea, count int)"))
    else:
        return ((index_config.get_value('coarse_table_name'),"(id serial PRIMARY KEY, vector float4[], count int)"),
            (index_config.get_value('fine_table_name'),"(id serial PRIMARY KEY, coarse_tag integer, word_id integer, vector int[])"),
            (index_config.get_value('cb_table_name'), "(id serial PRIMARY KEY, pos int, code int, vector float4[], count int)"))

def create_index_data(vectors, cq, codebook, logger):
    logger.log(Logger.INFO, 'len of vectors ' + str(len(vectors)))
    result = []
    indices = []
    coarse_counts = dict()
    fine_counts = dict()
    m = len(codebook)
    len_centr = int(len(vectors[0]) / m)

    # create faiss index for coarse quantizer
    coarse = faiss.IndexFlatL2(len(vectors[0]))
    coarse.add(cq)

    # create indices for codebook
    for i in range(m):
        index = faiss.IndexFlatL2(len_centr)
        index.add(codebook[i])
        indices.append(index)
    count = 0
    batches = [[] for i in range(m)]
    orders = []
    batch_coarse_ids = []
    for c in range(len(vectors)):
        # count += 1
        vec = vectors[c]
        # print('len(cq)', len(cq))
        _, I = coarse.search(np.array([vec]), len(cq))

        # coarse_ids.append(I[0]) #??

        # update coarse counts
        if I[0][0] in coarse_counts:
            coarse_counts[I[0][0]] += 1
        else:
            coarse_counts[I[0][0]] = 1
        for order, id in enumerate(I[0]):
            coarse_quantization = cq[id]
            batch_coarse_ids.append(id)
            residual = vec - coarse_quantization
            count += 1
            partition = np.array([np.array(residual[i:i + len_centr]).astype('float32') for i in range(0, len(residual), len_centr)])
            time1 = time.time()
            for i in range(m):
                if (time.time() - time1) > 60:
                    logger.log(Logger.INFO, 'vec ' + str(vec) + ' i ' +  str(i) +  ' m ' + str(m) + ' count ' + str(count))
                    time1 += 100000
                batches[i].append(partition[i])
            orders.append(order)
            if (count % 18 == 0) or (count == (len(vectors)*len(cq))): #  seems to be a good value
                # print('count', count, 'c', c,'order', order, 'batch_id_size', len(batch_coarse_ids))
                size = 18 if (count % 18 == 0) else len(batch_coarse_ids)
                # print('size', size, 'orders', orders, 'batch_size', len(batches[0]))
                #print('size', size, coarse_ids, 'c', c, len(vectors))
                # print('batch_coarse_ids[i]',batch_coarse_ids[i])
                codes=[(batch_coarse_ids[i],[], orders[i]) for i in range(size)]
                for i in range(m):
                    _, I = indices[i].search(np.array(batches[i]), 1)
                    for j in range(len(codes)):
                        codes[j][1].append(I[j][0])
                        if order == 0:
                            if (i, I[j][0]) in fine_counts:
                                fine_counts[(i, I[j][0])] += 1
                            else:
                                fine_counts[(i, I[j][0])] = 1
                result += codes
                batches = [[] for i in range(m)]
                orders = []
                batch_coarse_ids = []
                # if order == (len(cq)-1):
                #     coarse_ids = []
            if count % 1000 == 0:
                logger.log(Logger.INFO, 'Appended ' + str(len(result)) + ' vectors')
    logger.log(Logger.INFO, 'Appended ' + str(len(result)) + ' vectors')
    return result, coarse_counts, fine_counts

def add_to_database(words, cq, codebook, pq_quantization, coarse_counts, fine_counts, con, cur, index_config, batch_size, logger):
    # add codebook
    db_export.add_codebook_to_database(codebook, fine_counts, con, cur, index_config)

    # add coarse quantization
    db_export.add_cq_to_database(cq, coarse_counts, con, cur, index_config)

    # add fine qunatization
    values = []
    for i in range(len(pq_quantization)):
        output_vec = utils.serialize_vector(pq_quantization[i][1])
        # print('pq_quantization[i]', pq_quantization[i])
        values.append({"coarse_tag": str(pq_quantization[i][0]*len(cq)+pq_quantization[i][2]), "word_id": (i // len(cq))+1, "vector": output_vec})
        # print(values[0])
        if (i % (batch_size-1) == 0) or (i == (len(pq_quantization)-1)):
            if USE_BYTEA_TYPE:
                cur.executemany("INSERT INTO "+ index_config.get_value('fine_table_name') + " (coarse_tag, word_id,vector) VALUES (%(coarse_tag)s, %(word_id)s, vec_to_bytea(%(vector)s::int2[]))", tuple(values))
            else:
                cur.executemany("INSERT INTO "+ index_config.get_value('fine_table_name') + " (coarse_tag, word_id,vector) VALUES (%(coarse_tag)s, %(word_id)s, %(vector)s)", tuple(values))
            con.commit()
            logger.log(Logger.INFO, 'Inserted ' +  str(i+1) + ' vectors')
            values = []
    return

def main(argc, argv):
    db_config = Configuration('config/db_config.json')
    logger = Logger(db_config.get_value('log'))
    if argc < 2:
        logger.log(Logger.ERROR, 'Configuration file for index creation required')
        return
    index_config = Configuration(argv[1])

    batch_size = db_config.get_value("batch_size")

    train_size_coarse = index_config.get_value('train_size_coarse')
    train_size_fine = index_config.get_value('train_size_fine')
    centr_num_coarse = index_config.get_value('k_coarse')
    m = index_config.get_value('m')
    k = index_config.get_value('k')

    # get vectors
    words, vectors, vectors_size = utils.get_vectors(index_config.get_value('vec_file_path'), logger)
    logger.log(logger.INFO, 'vectors_size :' + str(vectors_size))

    # determine coarse quantizer
    cq = None
    cq_filename = index_config.get_value('coarse_quantizer_file') if \
        index_config.has_key('coarse_quantizer_file') else None
    cq_output_name = cq_filename if cq_filename != None else 'coarse_quantizer.pcl'
    cq = qcreator.construct_quantizer(qcreator.create_coarse_quantizer,
        (vectors[:train_size_coarse], centr_num_coarse), logger,
        input_name=cq_filename, output_name=cq_output_name)

    # determine resiual codebook
    codebook = None
    codebook_filename = index_config.get_value('residual_codebook_file') if \
        index_config.has_key('residual_codebook_file') else None
    codebook_output_name = codebook_filename if codebook_filename != None else 'residual_codebook.pcl'
    codebook = qcreator.construct_quantizer(qcreator.create_residual_quantizer,
        (cq, vectors[:train_size_fine], m, k, logger), logger,
        input_name=codebook_filename, output_name=codebook_output_name)

    # create db connection
    con, cur = db_export.create_connection(db_config, logger)

    # preapare database
    utils.init_tables(con, cur, get_table_information(index_config), logger)
    utils.disable_triggers(index_config.get_value('fine_table_name'),con, cur)

    # create index with quantizers
    logger.log(logger.INFO, 'Start index creation (single cycle)')
    start = time.time()
    index, coarse_counts, fine_counts = create_index_data(vectors[:vectors_size], cq, codebook, logger)
    end = time.time()
    logger.log(logger.INFO, 'Finish index creation after ' + str(end - start) + ' seconds')
    # add to database
    add_to_database(words, cq, codebook, index, coarse_counts, fine_counts, con, cur, index_config, batch_size, logger)
    logger.log(logger.INFO, 'Create database index structures')
    utils.create_index(index_config.get_value('fine_table_name'), index_config.get_value('fine_word_index_name'), 'word_id', con, cur, logger)
    utils.create_index(index_config.get_value('fine_table_name'), index_config.get_value('fine_coarse_index_name'), 'coarse_tag', con, cur, logger)
    utils.enable_triggers(index_config.get_value('fine_table_name'), con, cur)

if __name__ == "__main__":
	main(len(sys.argv), sys.argv)
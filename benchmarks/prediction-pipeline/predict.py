try:
  import unzip_requirements
except ImportError:
  pass

import os
import json
import pickle
import boto3
import tensorflow as tf
import numpy as np
import time

FILE_DIR = '/tmp'
BUCKET = os.environ['BUCKET']
FOLDER = os.environ['FOLDER']
MODEL  = os.environ['MODEL']
RESIZE_IMAGE = os.environ['RESIZE_IMAGE']

def timestamp(response, event, startTime, endTime):
    stampBegin = 1000*time.time()
    prior = event['duration'] if 'duration' in event else 0
    response['duration']     = prior + endTime - startTime
    response['workflowEndTime'] = endTime
    response['workflowStartTime'] = event['workflowStartTime'] if 'workflowStartTime' in event else startTime
    priorCost = event['timeStampCost'] if 'timeStampCost' in event else 0
    response['timeStampCost'] = priorCost - (stampBegin-1000*time.time())
    return response

def predictHandler(event, context):
    #Use S3 to communicate big messages
    #######################################################################################################################
    s3 = boto3.client('s3', aws_access_key_id="your_aws_access_id_here",
                      aws_secret_access_key="your_aws_secret_access_key_here",
                      region_name="us-east-1")
    resize_pickle = s3.get_object(Bucket = BUCKET, Key = os.path.join(FOLDER, RESIZE_IMAGE))['Body'].read()
    #######################################################################################################################
    startTime = 1000*time.time()
    img = pickle.loads(resize_pickle)
    gd = tf.GraphDef.FromString(open('data/mobilenet_v2_1.0_224_frozen.pb', 'rb').read())

    inp, predictions = tf.import_graph_def(gd,  return_elements = ['input:0', 'MobilenetV2/Predictions/Reshape_1:0'])

    with tf.Session(graph=inp.graph):
        x = predictions.eval(feed_dict={inp: img})

    response = {
        "statusCode": 200,
        "body": json.dumps({'predictions': x.tolist()})
    }

    endTime = 1000*time.time()
    return timestamp(response, event, startTime, endTime)

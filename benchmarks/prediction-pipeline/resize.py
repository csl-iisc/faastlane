try:
  import unzip_requirements
except ImportError:
  pass

import os
import json
import time

import boto3
import numpy as np
from PIL import Image
import pickle

FILE_DIR = '/tmp'
BUCKET = os.environ['BUCKET']
FOLDER = os.environ['FOLDER']
IMAGE  = os.environ['IMAGE']
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

def resizeHandler(event, context):
    startTime = 1000*time.time()
    image = Image.open("data/image.jpg")
    img = np.array(image.resize((224, 224))).astype(np.float) / 128 - 1
    resize_img = img.reshape(1, 224,224, 3)

    response = {"statusCode": 200}
    serialized_resize = pickle.dumps(resize_img)
    endTime = 1000*time.time()
    #Baseline allows 1MB messages to be shared, use S3 to communicate messages
    #######################################################################################################################
    s3 = boto3.client('s3', aws_access_key_id="your_aws_access_id_here",
                      aws_secret_access_key="your_aws_secret_access_key_here",
                      region_name="us-east-1")

    s3response = s3.put_object(Bucket = BUCKET, Key = os.path.join(FOLDER, RESIZE_IMAGE), Body = serialized_resize)
    #######################################################################################################################
    return timestamp(response, event, startTime, endTime)

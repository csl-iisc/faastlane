try:
  import unzip_requirements
except ImportError:
  pass

import json
import numpy as np
import time

def timestamp(response, event, startTime, endTime):
    stampBegin = 1000*time.time()
    prior = event['duration'] if 'duration' in event else 0
    response['duration']     = prior + endTime - startTime
    response['workflowEndTime'] = endTime
    response['workflowStartTime'] = event['workflowStartTime'] if 'workflowStartTime' in event else startTime
    priorCost = event['timeStampCost'] if 'timeStampCost' in event else 0
    response['timeStampCost'] = priorCost - (stampBegin-1000*time.time())
    return response

def renderHandler(event, context):
    startTime = 1000*time.time()
    body = json.loads(event['body'])
    x = np.array(body['predictions'])

    text = "Top 1 Prediction: " + str(x.argmax()) + str(x.max())
    print(text)

    response = {
        "statusCode": 200,
        "body": json.dumps({'render': text})
    }

    endTime = 1000*time.time()
    return timestamp(response, event, startTime, endTime)

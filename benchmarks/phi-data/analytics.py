try:
  import unzip_requirements
except ImportError:
  pass

import time
import nltk
nltk.data.path.append('nltk_data/')
from nltk.tokenize import word_tokenize
from util import *

def main(event,context):
    startTime = 1000*time.time()
    tokens = word_tokenize(event['body']['message'])
    response = {'statusCode': 200, "body":len(tokens)}
    endTime = 1000*time.time()
    return timestamp(response, event, startTime, endTime, 0)
